"""Minimal, dependency-free ACP (Agent Client Protocol) client.

Why this exists
---------------
`hermes-acp.exe` speaks newline-delimited JSON-RPC 2.0 on stdio. On a Chinese
Windows install its stdout is GBK while the wire format is nominally UTF-8, so
we decode defensively (UTF-8 first, GBK fallback) and always *send* pure-ASCII
JSON (`ensure_ascii=True`) to avoid encoding surprises.

G1 adds an optional ``args`` list so the same client can launch agents described
by an :class:`pager_bridge.profiles.AgentProfile` (Hermes, Cursor ACP, ...).

The client is intentionally synchronous + threaded: the ACP reader thread
dispatches notifications/approvals, while `request()` blocks the caller. The
bridge runs `session/prompt` on a worker thread so streaming events keep
flowing while the turn is in progress.

WorkBuddy's embedded CLI is a special case: its ACP endpoint is not stdio.  It
is a local streamable-HTTP server (``--serve --acp --port 0``) whose endpoint is
printed on stdout.  The ``workbuddy-http`` transport below launches that process,
optionally supplies the CLI's sidecar credential-bootstrap key through named
pipes, and speaks JSON-RPC over the local HTTP/SSE endpoint using the same
request/notification/approval callbacks as the stdio client.
"""
from __future__ import annotations

import base64
import collections
import hashlib
import http.client
import json
import logging
import os
import queue
import re
import secrets
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

log = logging.getLogger("pager.acp")

BASE = Path(__file__).resolve().parent
DEFAULT_WORKBUDDY_KEY_FILE = BASE / "data" / "workbuddy" / "credential-key.json"
_WORKBUDDY_ENDPOINT_RE = re.compile(r"http://127\.0\.0\.1:\d+")


class ACPError(RuntimeError):
    pass


@dataclass
class _Pending:
    q: queue.Queue = field(default_factory=lambda: queue.Queue(maxsize=1))


class _WorkBuddySidecar:
    """The parent side of WorkBuddy's named-pipe credential bootstrap.

    The WorkBuddy CLI starts two Windows named-pipe endpoints when it is given
    the ``CODEBUDDY_SIDECAR_*`` environment variables:

    * a credential bootstrap endpoint that the CLI listens on, to which the
      parent connects and writes a one-line JSON bootstrap; and
    * a readiness endpoint that the CLI connects to, to which the parent
      replies with a JSON-RPC acknowledgement.

    Supplying the same locally generated 32-byte key on every launch lets the
    CLI encrypt its own auth fields at rest without touching the desktop app's
    auth file or its keyblob.
    """

    def __init__(self, key_path: Path):
        self.key_path = key_path
        self.session_id = uuid.uuid4().hex
        self.token = secrets.token_hex(32)
        suffix = uuid.uuid4().hex
        self.ready_path = "\\\\.\\pipe\\pager-wb-ready-" + suffix
        self.cred_path = "\\\\.\\pipe\\pager-wb-cred-" + suffix
        self.key, self.key_id = self._load_or_create_key()
        self._ready_handle = None
        self._modules = None
        self._thread = None

    def _load_or_create_key(self) -> tuple[bytes, str]:
        if self.key_path.exists():
            try:
                data = json.loads(self.key_path.read_text(encoding="utf-8"))
                text = str(data.get("keyBase64") or "")
                key = base64.b64decode(text, validate=True)
                key_id = str(data.get("keyId") or "")
            except Exception as exc:
                raise ACPError(f"WorkBuddy 凭据密钥文件无效：{self.key_path}（{exc}）") from exc
            if len(key) != 32:
                raise ACPError(f"WorkBuddy 凭据密钥长度错误：{self.key_path}")
            expected = hashlib.sha256(key).hexdigest()[:16]
            if key_id != expected or base64.b64encode(key).decode("ascii") != text:
                raise ACPError(f"WorkBuddy 凭据密钥校验失败：{self.key_path}")
            return key, expected

        key = secrets.token_bytes(32)
        key_id = hashlib.sha256(key).hexdigest()[:16]
        self.key_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "version": 1,
            "keyId": key_id,
            "keyBase64": base64.b64encode(key).decode("ascii"),
        }
        self.key_path.write_text(json.dumps(payload), encoding="utf-8")
        try:
            os.chmod(self.key_path, 0o600)
        except OSError:
            pass
        return key, key_id

    def env(self) -> dict[str, str]:
        return {
            "CODEBUDDY_SIDECAR_READY_SOCKET": self.ready_path,
            "CODEBUDDY_SIDECAR_CREDENTIAL_BOOTSTRAP_SOCKET": self.cred_path,
            "CODEBUDDY_SIDECAR_READY_TOKEN": self.token,
            "CODEBUDDY_SIDECAR_READY_SESSION_ID": self.session_id,
            "WORKBUDDY_AT_REST_ENCRYPTION": "fields",
        }

    def create_ready_pipe(self) -> None:
        try:
            import pywintypes
            import win32file
            import win32pipe
        except ImportError as exc:
            raise ACPError("WorkBuddy 凭据引导需要 pywin32（win32pipe）") from exc
        self._modules = (win32file, win32pipe, pywintypes)
        self._ready_handle = win32pipe.CreateNamedPipe(
            self.ready_path,
            win32pipe.PIPE_ACCESS_DUPLEX,
            win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_READMODE_BYTE | win32pipe.PIPE_WAIT,
            1,
            65536,
            65536,
            0,
            None,
        )
        self._thread = threading.Thread(target=self._ready_loop, name="wb-ready-pipe", daemon=True)
        self._thread.start()

    def start(self, pid: int) -> None:
        threading.Thread(target=self._bootstrap_loop, args=(pid,), name="wb-cred-pipe", daemon=True).start()

    def _ready_loop(self) -> None:
        assert self._modules
        win32file, win32pipe, pywintypes = self._modules
        try:
            try:
                win32pipe.ConnectNamedPipe(self._ready_handle, None)
            except pywintypes.error as exc:
                # 535 = ERROR_PIPE_CONNECTED: the client connected between
                # CreateNamedPipe and ConnectNamedPipe.
                if getattr(exc, "winerror", None) != 535:
                    raise
            data = b""
            while b"\n" not in data:
                _, chunk = win32file.ReadFile(self._ready_handle, 65536)
                if not chunk:
                    return
                data += chunk
            line = data.split(b"\n", 1)[0]
            msg = json.loads(line.decode("utf-8", "replace"))
            ack = json.dumps({
                "jsonrpc": "2.0",
                "id": msg.get("id"),
                "result": {"ok": True},
            }, separators=(",", ":")) + "\n"
            win32file.WriteFile(self._ready_handle, ack.encode("utf-8"))
        except Exception as exc:
            log.debug("WorkBuddy readiness pipe failed: %s", exc)
        finally:
            try:
                win32file.CloseHandle(self._ready_handle)
            except Exception:
                pass

    def _bootstrap_loop(self, pid: int) -> None:
        deadline = time.monotonic() + 4.5
        stream = None
        while time.monotonic() < deadline:
            try:
                stream = open(self.cred_path, "r+b", buffering=0)
                break
            except OSError:
                time.sleep(0.05)
        if stream is None:
            log.warning("WorkBuddy credential bootstrap pipe did not become available")
            return
        payload = {
            "bootstrap": {
                "version": 1,
                "policy": "fields",
                "symmetricKey": {
                    "keyId": self.key_id,
                    "keyBase64": base64.b64encode(self.key).decode("ascii"),
                },
                "asymmetricUnavailable": "missing-key",
            },
            "pid": pid,
            "sessionId": self.session_id,
            "token": self.token,
            "version": 1,
        }
        try:
            stream.write((json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"))
            stream.flush()
            ack = stream.readline()
            if not ack:
                log.warning("WorkBuddy credential bootstrap returned no acknowledgement")
            else:
                log.info("WorkBuddy credential bootstrap acknowledged")
        except Exception as exc:
            log.warning("WorkBuddy credential bootstrap failed: %s", exc)
        finally:
            try:
                stream.close()
            except Exception:
                pass

    def close(self) -> None:
        handle = self._ready_handle
        if handle is None:
            return
        try:
            import win32file
            win32file.CloseHandle(handle)
        except Exception:
            pass


class ACPClient:
    def __init__(
        self,
        exe: str,
        cwd: str,
        on_update: Callable[[dict[str, Any]], None],
        on_permission: Callable[[dict[str, Any], Any], None],
        stderr_path: str | None = None,
        env: dict[str, str] | None = None,
        args: list[str] | None = None,
        transport: str = "stdio",
        credential_bootstrap: bool = False,
        bootstrap_key_path: str | None = None,
    ) -> None:
        self.exe = exe
        self.args = list(args or [])
        self.cwd = cwd
        self.on_update = on_update
        self.on_permission = on_permission
        self.stderr_path = stderr_path
        self.transport = (transport or "stdio").strip().lower()
        if self.transport not in {"stdio", "workbuddy-http"}:
            raise ACPError(f"不支持的 ACP transport：{transport!r}")
        self.credential_bootstrap = bool(credential_bootstrap)
        key_path = bootstrap_key_path or os.environ.get("PAGER_WORKBUDDY_KEY_FILE")
        self.bootstrap_key_path = Path(key_path) if key_path else DEFAULT_WORKBUDDY_KEY_FILE
        self._env = dict(os.environ)
        self._env["PYTHONUTF8"] = "1"
        self._env["PYTHONIOENCODING"] = "utf-8"
        if env:
            self._env.update(env)

        self.proc: subprocess.Popen | None = None
        self._pending: dict[int, _Pending] = {}
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        self._next_id = 0
        self._closed = threading.Event()
        self._reader: threading.Thread | None = None
        self._stderr_thread: threading.Thread | None = None
        self._stderr_file = None

        # WorkBuddy streamable-HTTP state.
        self._http_base: str | None = None
        self._http_host = "127.0.0.1"
        self._http_port = 0
        self._http_connection_id: str | None = None
        self._http_session_token: str | None = None
        self._http_headers: dict[str, str] = {}
        self._endpoint_queue: queue.Queue = queue.Queue()
        self._stdout_tail: collections.deque[str] = collections.deque(maxlen=80)
        self._sidecar: _WorkBuddySidecar | None = None

    # ---- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        os.makedirs(self.cwd, exist_ok=True)
        if self.transport == "workbuddy-http":
            self._start_http()
            return

        stderr = subprocess.DEVNULL
        if self.stderr_path:
            self._stderr_file = open(self.stderr_path, "ab", buffering=0)
            stderr = self._stderr_file
        self.proc = subprocess.Popen(
            [self.exe, *self.args],
            cwd=self.cwd,
            env=self._env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=stderr,
            bufsize=0,
        )
        self._reader = threading.Thread(target=self._read_loop, name="acp-reader", daemon=True)
        self._reader.start()

    def close(self, timeout: float = 15.0) -> None:
        self._closed.set()
        if self._sidecar:
            self._sidecar.close()
        proc = self.proc
        if not proc:
            return
        if self.transport == "stdio":
            try:
                if proc.stdin:
                    proc.stdin.close()
            except OSError:
                pass
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
        else:
            try:
                if proc.poll() is None:
                    proc.terminate()
                proc.wait(timeout=min(8.0, timeout))
            except subprocess.TimeoutExpired:
                proc.kill()
            except OSError:
                pass
        if self._stderr_file:
            try:
                self._stderr_file.close()
            except OSError:
                pass
            self._stderr_file = None

    # ---- stdio wire I/O ----------------------------------------------------

    @staticmethod
    def _decode(raw: bytes) -> str:
        raw = raw.strip()
        if not raw:
            return ""
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            return raw.decode("gbk", errors="replace")

    def _send(self, msg: dict[str, Any]) -> None:
        if self.transport == "workbuddy-http":
            self._http_roundtrip(msg, None, timeout=8.0, expect_response=False,
                                 method_name="client response")
            return
        proc = self.proc
        if not proc or not proc.stdin:
            raise ACPError("ACP process is not running")
        # ASCII-only payload: safe regardless of the child's stdio encoding.
        data = (json.dumps(msg, ensure_ascii=True, separators=(",", ":")) + "\n").encode("ascii")
        with self._write_lock:
            proc.stdin.write(data)
            proc.stdin.flush()

    def _read_loop(self) -> None:
        assert self.proc and self.proc.stdout
        for raw in self.proc.stdout:
            line = self._decode(raw)
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                # ACP requires pure JSON on stdout; keep going instead of dying.
                continue

            if "method" in msg:
                if "id" in msg:
                    self._handle_server_request(msg)
                else:
                    self._dispatch_update(msg.get("params") or {})
                continue

            rid = msg.get("id")
            with self._lock:
                pending = self._pending.pop(rid, None)
            if pending:
                pending.q.put(msg)

        # stdout closed -> fail every waiter
        with self._lock:
            pending = list(self._pending.values())
            self._pending.clear()
        for p in pending:
            p.q.put({"error": {"code": -32000, "message": "ACP process exited"}})
        self._closed.set()

    def _handle_server_request(self, msg: dict[str, Any]) -> None:
        method = msg.get("method")
        rid = msg.get("id")
        if method == "session/request_permission":
            try:
                self.on_permission(msg.get("params") or {}, rid)
            except Exception:
                self._send({"jsonrpc": "2.0", "id": rid,
                            "result": {"outcome": {"outcome": "cancelled"}}})
            return
        # We advertise no fs/terminal capabilities; fail fast so the agent
        # doesn't wait for a client that will never answer.
        self._send({
            "jsonrpc": "2.0", "id": rid,
            "error": {"code": -32601, "message": f"client does not implement {method}"},
        })

    def _dispatch_update(self, params: dict[str, Any]) -> None:
        try:
            self.on_update(params)
        except Exception:
            log.debug("ACP update callback failed", exc_info=True)

    # ---- stdio RPC ---------------------------------------------------------

    def request(self, method: str, params: dict[str, Any], timeout: float = 120.0) -> dict[str, Any]:
        if self.transport == "workbuddy-http":
            return self._http_request(method, params, timeout)
        with self._lock:
            self._next_id += 1
            rid = self._next_id
            pending = _Pending()
            self._pending[rid] = pending
        self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        try:
            msg = pending.q.get(timeout=timeout)
        except queue.Empty:
            with self._lock:
                self._pending.pop(rid, None)
            raise ACPError(f"ACP {method} timed out after {timeout:.0f}s")
        if "error" in msg:
            err = msg["error"] or {}
            raise ACPError(f"ACP {method} failed: {err.get('message') or err}")
        return msg.get("result") or {}

    def initialize(self) -> dict[str, Any]:
        return self.request("initialize", {
            "protocolVersion": 1,
            "clientCapabilities": {
                "fs": {"readTextFile": False, "writeTextFile": False},
                "terminal": False,
            },
            "clientInfo": {"name": "pager-g1", "version": "0.2.0"},
        }, timeout=60)

    def new_session(self) -> dict[str, Any]:
        return self.request("session/new", {"cwd": self.cwd, "mcpServers": []}, timeout=240)

    def authenticate(self, method_id: str, timeout: float = 300.0) -> dict[str, Any]:
        """Run the adapter's interactive login flow.

        Cursor/Codex/WorkBuddy implement this by opening their browser login UI
        and resolving only after the user finishes.  The call therefore blocks
        for as long as the user needs; the bridge keeps the UI in a login-wait
        state during that time.
        """
        return self.request("authenticate", {"methodId": method_id}, timeout=timeout)

    def prompt(self, session_id: str, text: str, timeout: float = 900.0) -> dict[str, Any]:
        return self.request("session/prompt", {
            "sessionId": session_id,
            "prompt": [{"type": "text", "text": text}],
        }, timeout=timeout)

    def cancel(self, session_id: str) -> None:
        try:
            self.request("session/cancel", {"sessionId": session_id}, timeout=15)
        except Exception:
            pass

    def respond_permission(self, request_id: Any, option_id: str | None) -> None:
        if option_id:
            result = {"outcome": {"outcome": "selected", "optionId": option_id}}
        else:
            result = {"outcome": {"outcome": "cancelled"}}
        if self.transport == "workbuddy-http":
            self._http_send_response(request_id, result=result)
            return
        self._send({"jsonrpc": "2.0", "id": request_id, "result": result})

    # ---- WorkBuddy streamable HTTP transport -------------------------------

    def _start_http(self) -> None:
        stderr = subprocess.DEVNULL
        if self.stderr_path:
            self._stderr_file = open(self.stderr_path, "ab", buffering=0)
            stderr = self._stderr_file

        env = dict(self._env)
        if self.credential_bootstrap:
            for key in ("USERPROFILE", "HOME", "WORKBUDDY_CONFIG_DIR"):
                value = env.get(key)
                if value:
                    os.makedirs(value, exist_ok=True)
            self._sidecar = _WorkBuddySidecar(self.bootstrap_key_path)
            self._sidecar.create_ready_pipe()
            env.update(self._sidecar.env())

        self.proc = subprocess.Popen(
            [self.exe, *self.args],
            cwd=self.cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=stderr,
            bufsize=0,
        )
        if self._sidecar:
            self._sidecar.start(self.proc.pid)
        self._reader = threading.Thread(target=self._http_stdout_loop, name="wb-http-stdout", daemon=True)
        self._reader.start()
        endpoint = self._wait_http_endpoint(45.0)
        self._http_base = endpoint.rstrip("/")
        parsed = urllib.parse.urlsplit(self._http_base)
        self._http_host = parsed.hostname or "127.0.0.1"
        self._http_port = parsed.port or 80
        self._http_connect()

    def _http_stdout_loop(self) -> None:
        assert self.proc and self.proc.stdout
        for raw in self.proc.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            if not line:
                continue
            self._stdout_tail.append(line)
            match = _WORKBUDDY_ENDPOINT_RE.search(line)
            if match and self._http_base is None:
                self._endpoint_queue.put(match.group(0))
        if self._http_base is None:
            self._endpoint_queue.put(None)

    def _wait_http_endpoint(self, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                item = self._endpoint_queue.get(timeout=min(0.5, remaining))
            except queue.Empty:
                if self.proc and self.proc.poll() is not None:
                    break
                continue
            if item:
                return item
            break
        tail = "\n".join(self._stdout_tail)
        detail = f"；最近输出：{tail[-1200:]}" if tail else ""
        raise ACPError(f"WorkBuddy HTTP ACP 服务未启动{detail}")

    def _http_connect(self) -> None:
        assert self._http_base
        req = urllib.request.Request(
            self._http_base + "/api/v1/acp/connect",
            data=b"{}",
            method="POST",
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json",
                "x-codebuddy-request": "1",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                body = resp.read()
        except urllib.error.HTTPError as exc:
            detail = exc.read(4000).decode("utf-8", "replace")
            raise ACPError(f"WorkBuddy ACP connect 失败（HTTP {exc.code}）：{detail}") from exc
        except Exception as exc:
            raise ACPError(f"WorkBuddy ACP connect 失败：{exc}") from exc
        try:
            data = json.loads(body.decode("utf-8"))
        except Exception as exc:
            raise ACPError(f"WorkBuddy ACP connect 返回无效 JSON：{body[:500]!r}") from exc
        connection_id = data.get("connectionId")
        if not connection_id:
            raise ACPError(f"WorkBuddy ACP connect 未返回 connectionId：{data!r}")
        self._http_connection_id = str(connection_id)
        token = data.get("sessionToken")
        self._http_session_token = str(token) if token else None
        self._http_headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "x-codebuddy-request": "1",
            "acp-connection-id": self._http_connection_id,
        }
        if self._http_session_token:
            self._http_headers["acp-session-token"] = self._http_session_token

    def _http_request(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
        with self._lock:
            self._next_id += 1
            rid = self._next_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method, "params": params}
        return self._http_roundtrip(msg, rid, timeout=timeout, expect_response=True,
                                    method_name=method)

    def _http_roundtrip(
        self,
        msg: dict[str, Any],
        rid: Any,
        timeout: float,
        expect_response: bool,
        method_name: str,
    ) -> dict[str, Any]:
        assert self._http_base
        deadline = time.monotonic() + timeout
        conn = http.client.HTTPConnection(
            self._http_host,
            self._http_port,
            timeout=max(1.0, min(timeout, 30.0)),
        )
        try:
            body = json.dumps(msg, ensure_ascii=True, separators=(",", ":")).encode("ascii")
            conn.request("POST", "/api/v1/acp", body, headers=self._http_headers)
            resp = conn.getresponse()
            if resp.status != 200:
                detail = resp.read(4000).decode("utf-8", "replace")
                raise ACPError(f"WorkBuddy ACP {method_name} 失败（HTTP {resp.status}）：{detail}")
            if conn.sock:
                # The SSE stream can legitimately stay silent while a browser
                # login is pending.  Let one readline wait for the whole
                # request budget; polling with short socket timeouts leaves
                # Python's buffered reader in an unusable "timed out" state.
                conn.sock.settimeout(max(1.0, timeout))
            ctype = (resp.getheader("Content-Type") or "").lower()
            if "text/event-stream" in ctype:
                return self._http_read_sse(
                    resp, rid, deadline, expect_response, method_name
                )
            data = resp.read()
            if not data:
                if expect_response:
                    raise ACPError(f"WorkBuddy ACP {method_name} 返回空响应")
                return {}
            try:
                payload = json.loads(data.decode("utf-8"))
            except Exception as exc:
                raise ACPError(
                    f"WorkBuddy ACP {method_name} 返回无效 JSON：{data[:500]!r}"
                ) from exc
            if "method" in payload:
                if "id" in payload:
                    self._http_server_request(payload)
                else:
                    self._dispatch_update(payload.get("params") or {})
                if expect_response:
                    raise ACPError(f"WorkBuddy ACP {method_name} 返回了通知而不是响应")
                return {}
            if "error" in payload:
                raise self._http_error(method_name, payload["error"])
            return payload.get("result") or {}
        except ACPError:
            raise
        except (socket.timeout, TimeoutError) as exc:
            if expect_response:
                raise ACPError(f"WorkBuddy ACP {method_name} 超时") from exc
            return {}
        except Exception as exc:
            raise ACPError(f"WorkBuddy ACP {method_name} 通信失败：{exc}") from exc
        finally:
            try:
                conn.close()
            except Exception:
                pass

    def _http_read_sse(
        self,
        resp: http.client.HTTPResponse,
        rid: Any,
        deadline: float,
        expect_response: bool,
        method_name: str,
    ) -> dict[str, Any]:
        data_lines: list[str] = []
        while True:
            if time.monotonic() >= deadline:
                if expect_response:
                    raise ACPError(f"WorkBuddy ACP {method_name} 超时")
                return {}
            try:
                raw = resp.readline()
            except (socket.timeout, TimeoutError) as exc:
                if expect_response:
                    raise ACPError(f"WorkBuddy ACP {method_name} 超时") from exc
                return {}
            except OSError as exc:
                if time.monotonic() >= deadline:
                    if expect_response:
                        raise ACPError(f"WorkBuddy ACP {method_name} 超时") from exc
                    return {}
                raise ACPError(f"WorkBuddy ACP {method_name} SSE 读取失败：{exc}") from exc
            if not raw:
                break
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if line == "":
                if not data_lines:
                    continue
                data = "\n".join(data_lines)
                data_lines.clear()
                try:
                    payload = json.loads(data)
                except json.JSONDecodeError:
                    continue
                if "method" in payload:
                    if "id" in payload:
                        self._http_server_request(payload)
                    else:
                        self._dispatch_update(payload.get("params") or {})
                    continue
                if expect_response and payload.get("id") == rid:
                    if "error" in payload:
                        raise self._http_error(method_name, payload["error"])
                    return payload.get("result") or {}
                continue
            if line.startswith(":"):
                continue
            if line.startswith("data:"):
                data_lines.append(line[5:].lstrip())
            elif line.startswith(("event:", "id:", "retry:")):
                continue
            else:
                # Be tolerant of servers that emit a bare JSON line.
                try:
                    payload = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if "method" in payload:
                    if "id" in payload:
                        self._http_server_request(payload)
                    else:
                        self._dispatch_update(payload.get("params") or {})
                elif expect_response and payload.get("id") == rid:
                    if "error" in payload:
                        raise self._http_error(method_name, payload["error"])
                    return payload.get("result") or {}
        if expect_response:
            raise ACPError(f"WorkBuddy ACP {method_name} 连接在响应前关闭")
        return {}

    @staticmethod
    def _http_error(method_name: str, err: Any) -> ACPError:
        if isinstance(err, dict):
            detail = err.get("message") or str(err)
        else:
            detail = str(err)
        return ACPError(f"WorkBuddy ACP {method_name} 失败：{detail}")

    def _http_server_request(self, msg: dict[str, Any]) -> None:
        method = msg.get("method")
        rid = msg.get("id")
        if method == "session/request_permission":
            try:
                self.on_permission(msg.get("params") or {}, rid)
            except Exception:
                self._http_send_response(rid, result={"outcome": {"outcome": "cancelled"}})
            return
        self._http_send_response(
            rid,
            error={"code": -32601, "message": f"client does not implement {method}"},
        )

    def _http_send_response(
        self,
        rid: Any,
        result: dict[str, Any] | None = None,
        error: dict[str, Any] | None = None,
    ) -> None:
        msg: dict[str, Any] = {"jsonrpc": "2.0", "id": rid}
        if error is not None:
            msg["error"] = error
        else:
            msg["result"] = result if result is not None else {}
        try:
            self._http_roundtrip(
                msg, None, timeout=5.0, expect_response=False,
                method_name="client response",
            )
        except Exception:
            log.debug("WorkBuddy ACP client response failed", exc_info=True)
