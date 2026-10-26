"""Pager G1 bridge: Agent Profile (ACP) -> WebSocket/HTTP -> pager.

G1 keeps the PC UI on a localhost-only HTTP listener.  The device endpoint is a
separate HTTPS/WSS listener started by ``python -m pager_bridge.serve`` and is
protected by a paired bearer token.  The legacy single-server command below is
still useful for UI development without exposing a LAN port:

    python -m uvicorn pager_bridge.bridge:app --host 127.0.0.1 --port 8765

Then open http://127.0.0.1:8765/
"""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from . import tls as tlsutil
from .acp_client import ACPClient, ACPError
from .discovery import instance_name as mdns_instance_name
from .profiles import AgentProfile, ProfileError, ProfileRegistry
from .security import DeviceHub, DeviceStore, PairingError, PairingManager
from .speech import (
    normalize_voice,
    polish_utterance,
    speech_segments,
    synthesize_segment,
    take_sentences,
    transcribe_pcm,
)

log = logging.getLogger("pager.bridge")

BASE = Path(__file__).resolve().parent
WORKSPACE = BASE.parent / "pager_workspace"
ACTIVE_PROFILE_FILE = BASE / "active_profile"
STATIC = BASE / "static"
DATA_DIR = BASE / "data"
CERT_DIR = Path(os.environ.get("PAGER_CERT_DIR", str(BASE / "certs")))
DEVICES_FILE = Path(os.environ.get("PAGER_DEVICES_FILE", str(DATA_DIR / "devices.json")))
POLICY_FILE = DATA_DIR / "approval-policy.json"
_POLICY_LEVELS = ("L1", "L2", "L3")
AUDIT_FILE = Path(os.environ.get("PAGER_AUDIT_FILE", str(DATA_DIR / "audit.log")))
DEVICE_PORT = int(os.environ.get("PAGER_DEVICE_PORT", "8766"))
DEVICE_WS_PATH = "/device/ws"
DEVICE_PAIR_PATH = "/device/pair/confirm"

DEVICE_STORE = DeviceStore(DEVICES_FILE)
PAIRING = PairingManager(DEVICE_STORE, audit_path=AUDIT_FILE)
DEVICE_HUB = DeviceHub()

IDLE, BOOTING, THINKING, RUNNING_TOOL = "IDLE", "BOOTING", "THINKING", "RUNNING_TOOL"
WAITING_APPROVAL, DONE, ERROR, OFFLINE = "WAITING_APPROVAL", "DONE", "ERROR", "OFFLINE"

# ---------------------------------------------------------------------------
# Risk classification (L0 read -> L3); remote approve policy is separate
# ---------------------------------------------------------------------------

L3_PATTERNS = [
    r"\brm\s+(-[a-z]*[rf][a-z]*\s+)+[/~]",
    r"\bformat\s+[a-z]:",
    r"\bdiskpart\b", r"\bbcdedit\b", r"\breg\s+delete\b",
    r"\bshutdown\b", r"\brestart-computer\b", r"\bstop-computer\b",
    r"\bnet\s+user\b", r"\btakeown\b", r"\bicacls\b",
    r"powershell(\.exe)?.*\s-(enc|encodedcommand)\b",
    r"(curl|wget|iwr|invoke-webrequest).*\|\s*(ba)?sh\b",
    r"\.ssh\b", r"\bid_rsa\b", r"\bid_ed25519\b",
    r"\.env(\.|\s|$)", r"\bcredentials?\b", r"\bpassword\b", r"\bsecret\b",
    r"c:\\windows\b", r"c:\\program files\b",
    r"\bsudo\b", r"\bdel\s+/[sfq]\b", r"\brmdir\s+/s\b",
]

L2_PATTERNS = [
    r"\b(del|erase|rmdir|rd)\b", r"\bremove-item\b", r"\brm\b", r"\bmv\b", r"\bmove\b",
    r"\b(pip|pip3|npm|pnpm|yarn|winget|choco|scoop)\s+(install|add|uninstall|remove)\b",
    r"\bgit\s+(push|reset|clean|checkout\s+--)\b",
    r"\b(curl|wget|invoke-webrequest|iwr)\b",
    r"\b(ssh|scp|sftp)\b",
    r"\b(taskkill|kill|stop-process)\b",
    r"\breg\s+add\b", r"\bstart-process\b",
]

DRIVE_PATH_RE = re.compile(r"([A-Za-z]:\\[^\s\"'<>|]+)")

MACROS_FILE = DATA_DIR / "macros.json"
TOAST_LEVELS = frozenset({"info", "ok", "warn", "err"})
NOTIFY_LIMIT_PER_MIN = 10
RING_LIMIT_SECONDS = 5.0


class _SlidingRateLimit:
    def __init__(self, limit: int, window: float) -> None:
        self.limit = limit
        self.window = window
        self._events: dict[str, list[float]] = {}
        self._lock = threading.Lock()

    def allow(self, key: str) -> bool:
        now = time.time()
        with self._lock:
            stamps = [t for t in self._events.get(key, []) if now - t < self.window]
            if len(stamps) >= self.limit:
                self._events[key] = stamps
                return False
            stamps.append(now)
            self._events[key] = stamps
            return True


_notify_rate = _SlidingRateLimit(NOTIFY_LIMIT_PER_MIN, 60.0)
_ring_rate = _SlidingRateLimit(1, RING_LIMIT_SECONDS)


def load_macros() -> dict[str, dict[str, Any]]:
    try:
        raw = json.loads(MACROS_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(raw, dict):
        return {}
    out: dict[str, dict[str, Any]] = {}
    for key, spec in raw.items():
        if not isinstance(spec, dict):
            continue
        cmd = str(spec.get("cmd") or "").strip()
        if not cmd:
            continue
        out[str(key)] = {
            "label": str(spec.get("label") or key)[:40],
            "cmd": cmd,
            "cwd": str(spec.get("cwd") or "").strip(),
            "confirm": bool(spec.get("confirm")),
        }
    return out


def public_macros() -> list[dict[str, str]]:
    return [{"id": mid, "label": spec["label"]} for mid, spec in load_macros().items()]


def _run_macro_worker(macro_id: str, spec: dict[str, Any]) -> None:
    label = spec.get("label") or macro_id
    bridge.push_toast("宏", f"正在运行 · {label}", level="info", ttl_ms=4000)
    cwd = spec.get("cwd") or str(bridge.workspace)
    try:
        proc = subprocess.run(
            spec["cmd"],
            shell=True,
            cwd=cwd or None,
            capture_output=True,
            text=True,
            timeout=600,
        )
        ok = proc.returncode == 0
        tail = (proc.stderr or proc.stdout or "").strip().replace("\r", " ")
        if len(tail) > 120:
            tail = tail[:119] + "…"
        body = f"{label} · exit {proc.returncode}"
        if tail:
            body = f"{body}\n{tail}"
        bridge.push_toast(
            "宏完成" if ok else "宏失败",
            body,
            level="ok" if ok else "err",
            ttl_ms=8000,
        )
        PAIRING.audit("macro.run", ok=ok, macro_id=macro_id, exit=proc.returncode)
    except subprocess.TimeoutExpired:
        bridge.push_toast("宏超时", label, level="err", ttl_ms=8000)
        PAIRING.audit("macro.run", ok=False, macro_id=macro_id, error="timeout")
    except Exception as exc:
        bridge.push_toast("宏失败", str(exc)[:200], level="err", ttl_ms=8000)
        PAIRING.audit("macro.run", ok=False, macro_id=macro_id, error=str(exc)[:120])


# L2 and L3 can both be approved on the watch face.

# Screen-first summaries: the device shows "运行命令 · taskkill…", never the raw pipeline.
_TOOL_KIND_LABEL = {
    "read": "读取", "search": "搜索", "edit": "改文件", "execute": "运行命令",
    "fetch": "联网", "delete": "删除", "move": "移动", "think": "思考", "other": "工具",
}


def tool_summary(title: str, kind: str, limit: int = 26) -> str:
    label = _TOOL_KIND_LABEL.get((kind or "").lower(), "工具")
    text = re.sub(r"\s+", " ", (title or "").strip())
    name, sep, rest = text.partition(":")
    target = rest.strip() if sep and len(name) <= 24 and rest.strip() else text
    if target:
        # Keep only the first meaningful token: "taskkill /FO CSV …" -> "taskkill"
        target = target.split(" ", 1)[0]
    if len(target) > limit:
        target = target[: limit - 1] + "…"
    return f"{label} · {target}" if target else label


_FILE_VERBS = {"edit": "修改文件", "read": "读取文件", "delete": "删除文件", "move": "移动文件"}


def approval_summary(title: str, kind: str, command: str = "", paths: list[str] | None = None) -> str:
    """One-line approval label that fits a 240x284 card.

    File approvals show the basename (the device never needs the full path);
    command approvals show the first token (``rm`` / ``taskkill``), not the
    whole pipeline.
    """
    kind = (kind or "").lower()
    if kind in _FILE_VERBS:
        target = str(paths[0]) if paths else ""
        if not target:
            blob = f"{title or ''} {command or ''}"
            m = re.search(r"([A-Za-z]:\\[^\s\"'<>|]+|[^\s:]+[/\\][^\s]+)", blob)
            if m:
                target = m.group(1)
        if target:
            p = target.rstrip("\\/")
            name = os.path.basename(p) or p
            return f"{_FILE_VERBS[kind]} · {name}"
        return tool_summary(title, kind)
    if kind in ("execute", "terminal") or command:
        first = (command or title or "").strip().split(" ", 1)[0]
        if first:
            return f"运行命令 · {first}"
    return tool_summary(title, kind)


def _find_git_bash() -> str | None:
    """Hermes' Windows file/terminal tools shell out through Git Bash."""
    candidates = [
        os.environ.get("HERMES_GIT_BASH_PATH"),
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
        str(Path.home() / "PortableGit" / "bin" / "bash.exe"),
        shutil.which("bash"),
    ]
    for c in candidates:
        if c and "System32" not in c and Path(c).is_file():
            return c
    return None


def _within(path: str, cwd: str) -> bool:
    try:
        p = Path(path).expanduser()
        if not p.is_absolute():
            p = Path(cwd) / p
        return p.resolve(strict=False).is_relative_to(Path(cwd).resolve(strict=False))
    except Exception:
        return False


def classify_risk(command: str, kind: str, paths: list[str], cwd: str) -> tuple[str, str]:
    text = command or ""
    for pat in L3_PATTERNS:
        if re.search(pat, text, re.IGNORECASE):
            return "L3", f"命中高危模式：{pat}"
    for path in paths:
        if not _within(path, cwd):
            return "L3", f"目标在工作区外：{path}"
    for m in DRIVE_PATH_RE.finditer(text):
        if not _within(m.group(1), cwd):
            return "L3", f"命令涉及工作区外路径：{m.group(1)}"

    if kind == "delete":
        return "L2", "删除类操作"
    if kind in ("edit", "move"):
        return "L1", "工作区内文件修改"
    if kind == "fetch":
        return "L2", "访问外部网络"
    if kind == "execute":
        for pat in L2_PATTERNS:
            if re.search(pat, text, re.IGNORECASE):
                return "L2", f"系统/网络/删除类操作：{pat}"
        return "L2", "执行 shell 命令"
    if kind in ("read", "search"):
        return "L0", "只读操作"
    for pat in L2_PATTERNS:
        if re.search(pat, text, re.IGNORECASE):
            return "L2", f"系统/网络/删除类操作：{pat}"
    return "L1", "默认"


_LOGIN_METHOD_PREFERENCE = (
    "chat-gpt",
    "cursor_login",
    "internal",
    "external",
    "iOA",
    "selfhosted",
)
_WORKBUDDY_SIGNED_IN = BASE / "data" / "workbuddy" / "signed-in"
_SKIP_LOGIN_IDS = {"api-key", "gateway"}


def auth_required_text(text: str) -> bool:
    return "Authentication required" in text or "Not logged in" in text


def prompt_refusal_text(result: dict[str, Any]) -> str | None:
    """Turn a prompt refusal into a sentence. A normal end_turn stays None."""
    if not isinstance(result, dict):
        return None
    reason = str(result.get("stopReason") or "")
    meta = result.get("_meta") if isinstance(result.get("_meta"), dict) else {}
    raw = result.get("errorMessage") or meta.get("codebuddy.ai/errorMessage") or ""
    message = ""
    if isinstance(raw, str):
        raw = raw.strip()
        if raw.startswith("{") or raw.startswith("["):
            try:
                parsed = json.loads(raw)
            except json.JSONDecodeError:
                parsed = None
            if isinstance(parsed, dict):
                err = parsed.get("error")
                if isinstance(err, dict) and err.get("message"):
                    message = str(err["message"])
                elif parsed.get("message"):
                    message = str(parsed["message"])
        if not message:
            message = raw
    elif raw:
        message = str(raw)
    if reason != "refusal" and not auth_required_text(message):
        return None
    if auth_required_text(message) or not message:
        return "WorkBuddy 需要先登录。桌面版开着，但这条命令行读不到那份登录，所以没有回复。"
    return message[:500]


def transport_dead_text(text: str) -> bool:
    """Cursor's agent keeps one HTTP/2 connection. The first turn works.

    The next turn verifies that idle connection with an HTTP/2 PING. Proxies
    and half-closed sockets answer with ``PING timed out`` or ``ECONNRESET``
    and the agent returns that string as the turn result.
    """
    if not text:
        return False
    if "PING timed out" in text:
        return True
    return any(mark in text for mark in ("ECONNRESET", "ECONNREFUSED", "socket hang up"))


def _mark_workbuddy_signed_in() -> None:
    _WORKBUDDY_SIGNED_IN.parent.mkdir(parents=True, exist_ok=True)
    _WORKBUDDY_SIGNED_IN.write_text("1\n", encoding="utf-8")


def _clear_workbuddy_signed_in() -> None:
    try:
        _WORKBUDDY_SIGNED_IN.unlink()
    except OSError:
        pass


def pick_login_method(auth_methods: Any, preferred: str = "") -> str | None:
    """Choose the ACP method that opens an interactive login.

    API-key and terminal setup methods are skipped: they do not open a login
    window. ``preferred`` wins when the agent actually advertised it.
    """
    usable: list[str] = []
    for item in auth_methods or []:
        if isinstance(item, str):
            method_id, method_type = item, ""
        elif isinstance(item, dict):
            method_id = str(item.get("id") or "")
            method_type = str(item.get("type") or "")
        else:
            continue
        if not method_id or method_type == "terminal" or method_id in _SKIP_LOGIN_IDS:
            continue
        usable.append(method_id)
    if preferred and preferred in usable:
        return preferred
    for method_id in _LOGIN_METHOD_PREFERENCE:
        if method_id in usable:
            return method_id
    return usable[0] if usable else None


def default_approval_policy() -> dict[str, bool]:
    return {level: False for level in _POLICY_LEVELS}


def _policy_bool(value: Any) -> bool:
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return bool(value)


def load_approval_policy() -> dict[str, bool]:
    policy = default_approval_policy()
    try:
        raw = json.loads(POLICY_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return policy
    if isinstance(raw, dict):
        for level in _POLICY_LEVELS:
            if level in raw:
                policy[level] = _policy_bool(raw[level])
    return policy


def save_approval_policy(policy: dict[str, bool]) -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    body = {level: bool(policy.get(level)) for level in _POLICY_LEVELS}
    POLICY_FILE.write_text(json.dumps(body, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


_HOST_DEFAULT: dict[str, float] = {
    "cpu": -1,
    "ramPct": -1,
    "ramUsed": -1,
    "ramTotal": -1,
    "gpu": -1,
    "vramUsed": -1,
    "vramTotal": -1,
    "gpuTemp": -1,
    "netDown": -1,
    "netUp": -1,
    "netPct": -1,
}

_HOST_NET_PREV: dict[str, float] = {"t": 0.0, "sent": 0.0, "recv": 0.0}
_NET_CAP_MBPS = 125.0


def collect_host_stats() -> dict[str, float]:
    stats = dict(_HOST_DEFAULT)
    try:
        import psutil

        stats["cpu"] = round(float(psutil.cpu_percent(interval=None)), 1)
        vm = psutil.virtual_memory()
        stats["ramPct"] = round(float(vm.percent), 1)
        stats["ramUsed"] = round(vm.used / (1024**3), 1)
        stats["ramTotal"] = round(vm.total / (1024**3), 1)
        import time

        io = psutil.net_io_counters()
        now = time.monotonic()
        prev_t = _HOST_NET_PREV["t"]
        if prev_t > 0:
            dt = now - prev_t
            if dt >= 0.4:
                down_mbps = max(
                    0.0,
                    (io.bytes_recv - _HOST_NET_PREV["recv"]) * 8.0 / dt / 1_000_000.0,
                )
                up_mbps = max(
                    0.0,
                    (io.bytes_sent - _HOST_NET_PREV["sent"]) * 8.0 / dt / 1_000_000.0,
                )
                stats["netDown"] = round(down_mbps, 1)
                stats["netUp"] = round(up_mbps, 1)
                peak = max(down_mbps, up_mbps)
                stats["netPct"] = round(min(100.0, peak / _NET_CAP_MBPS * 100.0), 1)
        _HOST_NET_PREV["t"] = now
        _HOST_NET_PREV["sent"] = float(io.bytes_sent)
        _HOST_NET_PREV["recv"] = float(io.bytes_recv)
    except Exception:
        log.debug("psutil host sample failed", exc_info=True)
    try:
        proc = subprocess.run(
            [
                "nvidia-smi",
                "--query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=2,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        if proc.returncode == 0 and proc.stdout.strip():
            row = proc.stdout.strip().splitlines()[0]
            parts = [p.strip() for p in row.split(",")]

            def _fnum(text: str) -> float:
                try:
                    return float(text)
                except ValueError:
                    return -1.0

            if len(parts) >= 4:
                stats["gpu"] = _fnum(parts[0])
                used = _fnum(parts[1])
                total = _fnum(parts[2])
                stats["vramUsed"] = round(used / 1024, 1) if used >= 0 else -1
                stats["vramTotal"] = round(total / 1024, 1) if total >= 0 else -1
                stats["gpuTemp"] = _fnum(parts[3])
    except Exception:
        pass
    return stats


_MEETING_ID_RE = re.compile(r"^\d{8}-\d{6}$")
_SESSION_LOG = DATA_DIR / "sessions" / "transcript.jsonl"


def append_session_history(entry: dict[str, Any], path: Path | None = None) -> None:
    target = path or _SESSION_LOG
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
    except OSError:
        log.exception("session history append failed")


def read_session_history(limit: int = 200, path: Path | None = None) -> list[dict[str, Any]]:
    limit = max(1, min(int(limit or 200), 500))
    target = path or _SESSION_LOG
    if not target.exists():
        return []
    try:
        lines = target.read_text(encoding="utf-8").splitlines()
    except OSError:
        log.exception("session history read failed")
        return []
    out: list[dict[str, Any]] = []
    for line in lines[-limit:]:
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def visible_transcript(
    memory: list[dict[str, Any]],
    saved: list[dict[str, Any]],
    limit: int = 60,
) -> list[dict[str, Any]]:
    """Disk history plus any live line that has not been flushed yet.

    Refresh reads the snapshot. The snapshot has to include the file, because
    the in-memory list is empty after a bridge restart.
    """
    items: list[dict[str, Any]] = []
    for index, row in enumerate(saved):
        text = str(row.get("text") or "").strip()
        if not text:
            continue
        items.append({
            "id": f"h{index}",
            "role": row.get("role") or "assistant",
            "text": text,
            "ts": row.get("ts") or 0,
            "profile": row.get("profile") or "",
            "profileName": row.get("profileName") or "",
        })
    last_ts = float(items[-1]["ts"] or 0) if items else 0.0
    for msg in memory:
        text = str(msg.get("text") or "").strip()
        ts = float(msg.get("ts") or 0)
        if not text or ts <= last_ts + 0.001:
            continue
        if items and items[-1].get("role") == msg.get("role") and items[-1].get("text") == text:
            continue
        items.append(msg)
    return items[-max(1, limit):]


def _meeting_title(mid: str) -> str:
    match = re.match(r"^(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})$", mid)
    if not match:
        return mid
    y, mo, d, h, mi, s = match.groups()
    return f"{y}-{mo}-{d} {h}:{mi}:{s}"


def list_meetings_index() -> list[dict[str, Any]]:
    folder = DATA_DIR / "meetings"
    if not folder.is_dir():
        return []
    items: list[dict[str, Any]] = []
    for path in sorted(folder.glob("*.json"), reverse=True):
        mid = path.stem
        if not _MEETING_ID_RE.match(mid):
            continue
        summary = ""
        line_count = 0
        try:
            body = json.loads(path.read_text(encoding="utf-8"))
            summary = str(body.get("summary") or "").strip()
            lines = body.get("lines") or []
            line_count = len(lines) if isinstance(lines, list) else 0
        except (OSError, json.JSONDecodeError):
            pass
        preview = summary.replace("\n", " ")[:120] if summary else ""
        items.append(
            {
                "id": mid,
                "title": _meeting_title(mid),
                "hasSummary": bool(summary),
                "summaryPreview": preview,
                "lineCount": line_count,
            }
        )
    return items


def load_meeting_record(mid: str) -> dict[str, Any] | None:
    if not _MEETING_ID_RE.match(mid):
        return None
    path = DATA_DIR / "meetings" / f"{mid}.json"
    if not path.is_file():
        return None
    try:
        body = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(body, dict):
        return None
    body.setdefault("id", mid)
    body["title"] = _meeting_title(mid)
    return body


def can_approve_remotely(risk: str, reason: str, kind: str, paths: list[str]) -> bool:
    """Whether the watch face may tap Allow.

    L2 and L3 are both allowed on the face. The arguments stay so callers
    can still pass the classified risk without a signature change.
    """
    del risk, reason, kind, paths
    return True


_COMMAND_RUNNING = {"in_progress", "running", "completed", "failed"}


def _command_running(status: Any) -> bool:
    return str(status or "").lower() in _COMMAND_RUNNING


def _pick_option(options: list[dict[str, Any]], decision: str) -> str | None:
    if decision == "allow":
        for o in options:
            if o.get("kind") == "allow_once":
                return o.get("optionId")
        for o in options:
            if str(o.get("kind", "")).startswith("allow"):
                return o.get("optionId")
        return None
    for o in options:
        if o.get("kind") == "reject_once":
            return o.get("optionId")
    for o in options:
        if str(o.get("kind", "")).startswith("reject"):
            return o.get("optionId")
    return None


# ---------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------

class Bridge:
    def __init__(self) -> None:
        self.loop: asyncio.AbstractEventLoop | None = None
        self.subscribers: set[asyncio.Queue] = set()
        self.client: ACPClient | None = None
        self._lock = threading.RLock()
        self._seq = 0
        self._generation = 0
        self.profile_error: str | None = None
        try:
            self.registry = ProfileRegistry.load()
        except ProfileError as exc:
            # Keep the known-good Hermes baseline available, but surface the
            # configuration error in snapshots instead of crashing the UI.
            self.registry = ProfileRegistry.default_registry()
            self.profile_error = str(exc)
        self.active_profile_id = self._saved_profile_id()
        self.profile: AgentProfile = self.registry.get(self.active_profile_id)
        self._remember_profile()
        self.workspace = Path(self.profile.cwd)
        self.state = BOOTING
        self.state_detail = f"正在启动 {self.profile.name} ACP…"
        self.session_id: str | None = None
        self.session_title = f"Pager G1 · {self.profile.name}"
        self.agent_info: dict[str, Any] = {}
        self.auth_methods: list[Any] = []
        self.transcript: list[dict[str, Any]] = []
        self.tools: dict[str, dict[str, Any]] = {}
        self.pending: dict[str, dict[str, Any]] = {}
        self.busy = False
        self.last_error: str | None = None
        self.usage: dict[str, Any] = {}
        self.metrics: dict[str, Any] = {
            "turns": 0,
            "approvals": 0,
            "lastFirstTokenMs": None,
            "lastTurnMs": None,
            "lastApprovalMs": None,
        }
        self._turn_t0: float | None = None
        self._first_token_at: float | None = None
        self._stream_text: list[str] = []
        self._assistant_msg_id: str | None = None
        self._assistant_streamed = False
        self._approval_seq = 0
        self._msg_seq = 0
        self.watch_volume: int | None = None
        self.watch_voice = "xiaoxiao"
        self.meeting: dict[str, Any] = {
            "on": False, "phase": "idle", "startedAt": None, "id": None, "lines": [],
        }
        self._meeting_save_id: str | None = None
        self.approval_policy = load_approval_policy()
        self.host_stats: dict[str, float] = dict(_HOST_DEFAULT)
        self._host_stop = threading.Event()
        self._host_thread: threading.Thread | None = None

    # ---- event bus ---------------------------------------------------------

    def attach_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self.loop = loop

    def subscribe(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue()
        self.subscribers.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self.subscribers.discard(q)

    def publish(self, event: dict[str, Any]) -> None:
        with self._lock:
            self._seq += 1
            event = {"seq": self._seq, "ts": time.time(), **event}
        loop = self.loop
        if not loop:
            return
        for q in list(self.subscribers):
            try:
                loop.call_soon_threadsafe(q.put_nowait, event)
            except RuntimeError:
                pass

    # ---- snapshot ----------------------------------------------------------

    def snapshot(self, *, for_device: bool = False) -> dict[str, Any]:
        with self._lock:
            full = {
                "type": "snapshot",
                "state": self.state,
                "stateDetail": self.state_detail,
                "sessionId": self.session_id,
                "sessionTitle": self.session_title,
                "agentInfo": self.agent_info,
                "transcript": visible_transcript(self.transcript, read_session_history(80)),
                "tools": list(self.tools.values())[-30:],
                "pending": list(self.pending.values()),
                "usage": self.usage,
                "metrics": dict(self.metrics),
                "lastError": self.last_error,
                "workspace": str(self.workspace),
                "profile": self.profile.runtime_public(),
                "profileError": self.profile_error,
                "watchVolume": self.watch_volume,
                "watchVoice": self.watch_voice,
                "meeting": self._meeting_public_locked(),
                "approvalPolicy": dict(self.approval_policy),
                "host": dict(self.host_stats),
                "macros": public_macros(),
            }
        if not for_device:
            return full
        text = ""
        for msg in reversed(full["transcript"]):
            if msg.get("role") == "assistant" and msg.get("text"):
                text = str(msg["text"])[:240]
                break
        return {
            "type": "snapshot",
            "state": full["state"],
            "stateDetail": str(full["stateDetail"] or "")[:80],
            "transcript": [{"role": "assistant", "text": text}] if text else [],
            "pending": full["pending"][:1],
            "macros": full["macros"],
            "host": dict(self.host_stats),
        }

    def _host_sample_loop(self) -> None:
        try:
            import psutil

            psutil.cpu_percent(interval=None)
        except Exception:
            log.warning("host sampler disabled: psutil unavailable")
            return
        while not self._host_stop.is_set():
            stats = collect_host_stats()
            with self._lock:
                self.host_stats = stats
            self.publish({"type": "host", **stats})
            if self._host_stop.wait(2.0):
                break

    def start_host_sampler(self) -> None:
        if self._host_thread and self._host_thread.is_alive():
            return
        self._host_stop.clear()
        self._host_thread = threading.Thread(
            target=self._host_sample_loop, name="host-stats", daemon=True
        )
        self._host_thread.start()

    def stop_host_sampler(self) -> None:
        self._host_stop.set()

    def _set_state(self, state: str, detail: str = "") -> None:
        with self._lock:
            self.state = state
            self.state_detail = detail
        self.publish({"type": "state", "state": state, "detail": detail})

    # ---- bootstrap ---------------------------------------------------------

    def start_async(self) -> None:
        with self._lock:
            self._generation += 1
            generation = self._generation
        threading.Thread(
            target=self.bootstrap, args=(generation,), name="agent-bootstrap", daemon=True
        ).start()

    def bootstrap(self, generation: int | None = None) -> None:
        with self._lock:
            profile = self.profile
            workspace = Path(profile.cwd)
            self.workspace = workspace
        self._set_state(BOOTING, f"正在启动 {profile.name} ACP…")
        client: ACPClient | None = None
        try:
            env: dict[str, str] = dict(profile.env)
            bash = _find_git_bash()
            if bash:
                env.setdefault("HERMES_GIT_BASH_PATH", bash)
            # NOTE: HERMES_EXEC_ASK is a gateway-only switch (needs a notify_cb).
            # ACP routes approvals through HERMES_INTERACTIVE + its own callback;
            # setting EXEC_ASK here did not gate execute_code and is not used.
            client = ACPClient(
                exe=profile.command,
                args=list(profile.args),
                cwd=str(workspace),
                on_update=self.on_update,
                on_permission=self.on_permission,
                stderr_path=str(BASE / "acp_stderr.log"),
                env=env,
                transport=profile.transport,
                credential_bootstrap=profile.transport == "workbuddy-http",
            )
            client.start()
            init = client.initialize()
            auth_methods = init.get("authMethods") or []
            with self._lock:
                self.auth_methods = list(auth_methods)
            try:
                session = client.new_session()
            except ACPError as exc:
                if not auth_required_text(str(exc)):
                    raise
                if self._stale(generation):
                    client.close(timeout=2)
                    return
                self._open_login(client, profile, auth_methods, generation)
                if self._stale(generation):
                    client.close(timeout=2)
                    return
                session = client.new_session()
            sid = session.get("sessionId")
            if not sid:
                raise ACPError("session/new returned no sessionId")
            with self._lock:
                if generation is not None and generation != self._generation:
                    # A profile switch/retry happened while this process was
                    # starting; discard the stale client.
                    try:
                        client.close(timeout=2)
                    except Exception:
                        pass
                    return
                self.client = client
                self.session_id = sid
                self.agent_info = init.get("agentInfo") or {}
                self.last_error = None
                self.profile_error = None
            self._set_state(IDLE, f"{profile.name} 已连接")
            self.publish({
                "type": "ready",
                "agentInfo": self.agent_info,
                "sessionId": sid,
                "profile": profile.runtime_public(),
            })
        except Exception as exc:
            if self._stale(generation):
                if client is not None:
                    try:
                        client.close(timeout=2)
                    except Exception:
                        pass
                return
            text = str(exc)
            if auth_required_text(text):
                text = f"{profile.name} 需要先在这台电脑上登录"
            with self._lock:
                self.last_error = text
            self._set_state(OFFLINE, f"连接失败：{text}")

    def _stale(self, generation: int | None) -> bool:
        return generation is not None and generation != self._generation

    def _open_login(
        self,
        client: ACPClient,
        profile: AgentProfile,
        auth_methods: Any,
        generation: int | None,
    ) -> None:
        """Open the agent's own login window and wait until it finishes."""
        self._set_state(BOOTING, f"正在打开 {profile.name} 的登录窗口…")
        if profile.login_command:
            self._run_login_command(profile, generation)
            if self._stale(generation):
                return
        method = pick_login_method(auth_methods, profile.auth_method)
        if method:
            log.info("opening %s login via %s", profile.name, method)
            client.authenticate(method, timeout=300)
            return
        if not profile.login_command:
            raise ACPError(f"{profile.name} 需要先在这台电脑上登录")

    def _run_login_command(self, profile: AgentProfile, generation: int | None) -> None:
        proc = subprocess.Popen(
            [profile.login_command, *profile.login_args],
            cwd=str(profile.cwd),
            env={**os.environ, **dict(profile.env)},
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 300
        try:
            while proc.poll() is None:
                if self._stale(generation):
                    proc.kill()
                    return
                if time.monotonic() > deadline:
                    proc.kill()
                    raise ACPError(f"{profile.name} 登录超时")
                time.sleep(0.3)
        finally:
            if proc.poll() is None and self._stale(generation):
                proc.kill()
        if proc.returncode not in (0, None):
            raise ACPError(f"{profile.name} 登录未完成")

    def retry(self) -> None:
        old = self.client
        if old:
            try:
                old.close(timeout=3)
            except Exception:
                pass
        with self._lock:
            self.client = None
            self.session_id = None
        self.start_async()

    def _saved_profile_id(self) -> str:
        fallback = self.registry.default_id
        try:
            saved = ACTIVE_PROFILE_FILE.read_text(encoding="utf-8").strip()
        except OSError:
            return fallback
        profile = self.registry.profiles.get(saved)
        if profile and profile.enabled:
            return profile.id
        return fallback

    def _remember_profile(self) -> None:
        try:
            ACTIVE_PROFILE_FILE.write_text(self.active_profile_id, encoding="utf-8")
        except OSError:
            pass

    def select_profile(self, profile_id: str) -> tuple[bool, str | None]:
        """Switch the active local agent without allowing an in-flight turn to
        be silently orphaned.  The admin endpoint is localhost-only."""
        with self._lock:
            if self.busy:
                return False, "当前有任务在执行，完成或取消后再切换"
            if self.pending:
                return False, "还有未处理的审批，处理后才能切换"
            try:
                profile = self.registry.get(profile_id)
            except ProfileError as exc:
                return False, str(exc)
            if not profile.enabled:
                return False, f"Profile {profile.name} 已禁用"
            old = self.client
            self.client = None
            self.session_id = None
            self.profile = profile
            self.active_profile_id = profile.id
            self.workspace = Path(profile.cwd)
            self.transcript = []
            self.tools = {}
            self.pending = {}
            self.usage = {}
            self.agent_info = {}
            self.auth_methods = []
            self.last_error = None
            self.session_title = f"Pager G1 · {profile.name}"
            self._set_state(BOOTING, f"正在切换到 {profile.name}…")
            self._remember_profile()
            self.publish({"type": "profile", "profile": profile.runtime_public()})
        if old:
            try:
                old.close(timeout=3)
            except Exception:
                pass
        self.start_async()
        return True, None

    # ---- ACP callbacks -----------------------------------------------------

    def _mark_command_started(self) -> None:
        """首字延迟从命令开始执行算，不从回复文字出现算。"""
        if self._first_token_at is None and self._turn_t0 is not None:
            self._first_token_at = time.monotonic()

    def on_update(self, params: dict[str, Any]) -> None:
        update = params.get("update") or {}
        kind = update.get("sessionUpdate")

        if kind == "agent_message_chunk":
            content = update.get("content") or {}
            if content.get("type") != "text":
                return
            text = content.get("text") or ""
            if not text:
                return
            self._stream_text.append(text)
            self._assistant_streamed = True
            if self._assistant_msg_id is None:
                self._assistant_msg_id = f"a{self._next_msg()}"
                with self._lock:
                    self.transcript.append({
                        "id": self._assistant_msg_id, "role": "assistant",
                        "text": "", "ts": time.time(),
                    })
            with self._lock:
                for msg in reversed(self.transcript):
                    if msg["id"] == self._assistant_msg_id:
                        msg["text"] += text
                        break
            if self.state != WAITING_APPROVAL:
                self._set_state(THINKING, f"{self.profile.name} 正在回复")
            self.publish({"type": "assistant_delta", "id": self._assistant_msg_id, "text": text})

        elif kind == "agent_thought_chunk":
            content = update.get("content") or {}
            if content.get("type") == "text" and content.get("text"):
                self.publish({"type": "thought_delta", "text": content["text"]})

        elif kind == "tool_call":
            title = update.get("title") or ""
            tool_kind = update.get("kind") or "other"
            tool = {
                "id": update.get("toolCallId") or f"t{self._next_msg()}",
                "title": title,
                "kind": tool_kind,
                "status": update.get("status") or "pending",
                "content": update.get("content") or [],
                "rawInput": update.get("rawInput") or {},
                "summary": tool_summary(title, tool_kind),
            }
            with self._lock:
                self.tools[tool["id"]] = tool
            if _command_running(tool["status"]):
                self._mark_command_started()
            if self.state != WAITING_APPROVAL:
                self._set_state(RUNNING_TOOL, tool["summary"])
            self.publish({"type": "tool", "tool": tool})

        elif kind == "tool_call_update":
            tid = update.get("toolCallId")
            summary = None
            with self._lock:
                tool = self.tools.get(tid or "")
                if tool:
                    for key in ("title", "kind", "status", "content", "rawInput"):
                        if key in update and update[key] is not None:
                            tool[key] = update[key]
                    if update.get("title") or update.get("kind"):
                        tool["summary"] = tool_summary(tool.get("title", ""), tool.get("kind", "other"))
                    summary = tool.get("summary")
                    running = _command_running(tool.get("status"))
                else:
                    running = _command_running(update.get("status"))
            if running:
                self._mark_command_started()
            self.publish({"type": "tool_update", "toolCallId": tid,
                          "status": update.get("status"), "title": update.get("title"),
                          "summary": summary})

        elif kind == "plan":
            self.publish({"type": "plan", "entries": update.get("entries") or []})

        elif kind == "usage_update":
            with self._lock:
                self.usage = {"used": update.get("used"), "size": update.get("size"),
                              "cost": update.get("cost")}
            self.publish({"type": "usage", "usage": self.usage})

        elif kind == "session_info_update":
            title = update.get("title")
            if title:
                with self._lock:
                    self.session_title = title
                self.publish({"type": "title", "title": title})

    def _next_msg(self) -> int:
        with self._lock:
            self._msg_seq += 1
            return self._msg_seq

    def on_permission(self, params: dict[str, Any], request_id: Any) -> None:
        tc = params.get("toolCall") or {}
        raw = tc.get("rawInput") or {}
        args = raw.get("arguments") or {}
        kind = str(tc.get("kind") or "other").lower()
        options = params.get("options") or []

        command = raw.get("command") or args.get("command") or ""
        description = raw.get("description") or tc.get("title") or ""
        paths: list[str] = []
        if args.get("path"):
            paths.append(str(args["path"]))
        if not command:
            for block in tc.get("content") or []:
                if isinstance(block, dict) and block.get("type") == "text":
                    command = block.get("text") or ""
                    break
                if isinstance(block, dict) and block.get("type") == "diff":
                    if block.get("path"):
                        paths.append(str(block["path"]))
        if not command:
            command = tc.get("title") or ""

        risk, reason = classify_risk(command, kind, paths, str(self.workspace))
        summary = approval_summary(tc.get("title") or "", kind, command, paths)
        option_rows = [{"optionId": o.get("optionId"), "kind": o.get("kind"),
                        "name": o.get("name")} for o in options]
        with self._lock:
            auto = risk in _POLICY_LEVELS and bool(self.approval_policy.get(risk))
        if auto:
            option_id = _pick_option(option_rows, "allow")
            if option_id and self.client:
                try:
                    self.client.respond_permission(request_id, option_id)
                except Exception:
                    log.exception("auto-approve failed, asking instead")
                else:
                    with self._lock:
                        self.metrics["approvals"] += 1
                        self.metrics["lastApprovalMs"] = 0
                    log.info("auto-approved %s %s", risk, summary)
                    self.publish({
                        "type": "approval_auto",
                        "risk": risk,
                        "summary": summary,
                        "latencyMs": 0,
                    })
                    return
        with self._lock:
            self._approval_seq += 1
            aid = f"apr-{self._approval_seq}"
            record = {
                "id": aid,
                "requestId": request_id,
                "toolCallId": tc.get("toolCallId"),
                "title": tc.get("title") or "需要批准",
                "kind": kind,
                "command": command,
                "description": description,
                "summary": summary,
                "risk": risk,
                "riskReason": reason,
                "canApprove": can_approve_remotely(risk, reason, kind, paths),
                "options": option_rows,
                "createdAt": time.time(),
                "paths": paths,
            }
            self.pending[aid] = record
        self._set_state(WAITING_APPROVAL, record["summary"])
        self.publish({"type": "approval", "approval": {k: v for k, v in record.items() if k != "requestId"}})

    # ---- user actions ------------------------------------------------------

    def _meeting_public_locked(self) -> dict[str, Any]:
        meeting = self.meeting
        return {
            "on": bool(meeting.get("on")),
            "phase": meeting.get("phase") or "idle",
            "startedAt": meeting.get("startedAt"),
            "id": meeting.get("id"),
            "lines": list(meeting.get("lines") or [])[-80:],
        }

    def _publish_meeting(self) -> None:
        with self._lock:
            event = {"type": "meeting", **self._meeting_public_locked()}
        self.publish(event)

    def start_meeting(self) -> tuple[bool, str | None]:
        with self._lock:
            if self.meeting.get("on") or self.meeting.get("phase") == "summarizing":
                return False, "会议还在记录或汇总"
            self.meeting = {
                "on": True,
                "phase": "recording",
                "startedAt": time.time(),
                "id": time.strftime("%Y%m%d-%H%M%S"),
                "lines": [],
                "finishing": False,
            }
        self._publish_meeting()
        log.info("meeting started")
        return True, None

    def stop_meeting(self) -> tuple[bool, str | None]:
        with self._lock:
            if not self.meeting.get("on"):
                return False, "没有正在记录的会议"
            mid = str(self.meeting.get("id") or "")
            self.meeting["on"] = False
            self.meeting["phase"] = "summarizing"
        self._publish_meeting()
        log.info("meeting stopping %s", mid)
        threading.Timer(12.0, self._finish_meeting, args=(mid,)).start()
        return True, None

    def note_meeting(self, text: str) -> None:
        text = (text or "").strip()
        if not text:
            return
        with self._lock:
            phase = self.meeting.get("phase")
            if phase not in {"recording", "summarizing"} or self.meeting.get("finishing"):
                return
            started = float(self.meeting.get("startedAt") or time.time())
            lines: list[dict[str, Any]] = self.meeting.setdefault("lines", [])
            lines.append({"t": round(time.time() - started, 1), "text": text})
            total = sum(len(str(line.get("text") or "")) for line in lines)
            while lines and total > 18000:
                total -= len(str(lines[0].get("text") or ""))
                lines.pop(0)
        self._publish_meeting()

    def _finish_meeting(self, mid: str) -> None:
        with self._lock:
            meeting = self.meeting
            if meeting.get("id") != mid or meeting.get("phase") != "summarizing":
                return
            if meeting.get("finishing"):
                return
            meeting["finishing"] = True
            lines = list(meeting.get("lines") or [])
            meeting["phase"] = "idle"
            meeting["on"] = False
        self._publish_meeting()
        body = "\n".join(str(line.get("text") or "").strip() for line in lines)
        body = "\n".join(part for part in body.split("\n") if part)
        self._write_meeting(mid, lines, "")
        if not body:
            self.publish({"type": "error", "message": "这次会议没有听到内容，没有生成纪要。"})
            return
        if len(body) > 18000:
            body = "（前面的记录已截断）\n" + body[-18000:]
        prompt = (
            "请把下面的会议口述整理成纪要，用中文。"
            "分成三部分：讨论了什么、决定了什么、还要做什么。"
            "只根据口述里出现的内容，没有提到的不要补。\n\n"
            "会议口述：\n" + body
        )
        self._meeting_save_id = mid
        ok, err = False, ""
        for _ in range(20):
            ok, err = self.submit_prompt(prompt)
            if ok or "执行" not in (err or ""):
                break
            time.sleep(2)
        if not ok:
            self._meeting_save_id = None
            self.publish({"type": "error", "message": err or "纪要没有送出去"})
            return
        log.info("meeting summary submitted %s", mid)

    def _write_meeting(self, mid: str, lines: list[dict[str, Any]], summary: str) -> None:
        folder = DATA_DIR / "meetings"
        try:
            folder.mkdir(parents=True, exist_ok=True)
            path = folder / f"{mid}.json"
            previous: dict[str, Any] = {}
            if path.exists() and summary:
                try:
                    previous = json.loads(path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    previous = {}
            payload = {
                "id": mid,
                "lines": previous.get("lines") or lines,
                "summary": summary or previous.get("summary") or "",
            }
            path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        except OSError:
            log.exception("meeting save failed")

    def submit_prompt(self, text: str) -> tuple[bool, str | None]:
        text = (text or "").strip()
        if not text:
            return False, "指令为空"
        if not self.session_id or not self.client:
            return False, f"{self.profile.name} 尚未连接"
        if self.busy:
            return False, "上一轮还在执行，请等它结束或先取消"
        with self._lock:
            self.busy = True
            self._turn_t0 = time.monotonic()
            self._first_token_at = None
            self._stream_text = []
            self._assistant_msg_id = None
            self._assistant_streamed = False
            self._msg_seq += 1
            uid = f"u{self._msg_seq}"
            self.transcript.append({"id": uid, "role": "user", "text": text, "ts": time.time()})
            profile_id = self.active_profile_id
        append_session_history(
            {
                "ts": time.time(),
                "role": "user",
                "text": text,
                "profile": profile_id,
                "profileName": self.profile.name,
            }
        )
        self.publish({"type": "user", "id": uid, "text": text})
        self._set_state(THINKING, f"已发送，等待 {self.profile.name}")
        threading.Thread(target=self._run_prompt, args=(text,), daemon=True).start()
        return True, None

    def _remember_assistant(self, text: str, meeting_id: str | None = None) -> None:
        text = (text or "").strip()
        if not text:
            return
        with self._lock:
            profile_id = self.active_profile_id
            profile_name = self.profile.name
        entry: dict[str, Any] = {
            "ts": time.time(),
            "role": "assistant",
            "text": text,
            "profile": profile_id,
            "profileName": profile_name,
        }
        if meeting_id:
            entry["meetingId"] = meeting_id
        append_session_history(entry)

    def _drop_dead_assistant(self) -> None:
        with self._lock:
            if self.transcript and self.transcript[-1].get("role") == "assistant":
                if transport_dead_text(str(self.transcript[-1].get("text") or "")):
                    self.transcript.pop()
            self._stream_text = []
            self._assistant_msg_id = None
            self._assistant_streamed = False

    def _recycle_agent(self) -> bool:
        """Replace the agent process so the next turn opens a fresh connection."""
        old = self.client
        with self._lock:
            generation = self._generation
            self.client = None
            self.session_id = None
        if old:
            try:
                old.close(timeout=3)
            except Exception:
                log.exception("close dead agent")
        self.bootstrap(generation)
        with self._lock:
            alive = self.client is not None and bool(self.session_id) and self.state != OFFLINE
        if not alive:
            log.warning("agent recycle failed: %s", self.last_error)
        return alive

    def _recover_transport(self, text: str) -> bool:
        self._drop_dead_assistant()
        self._set_state(THINKING, f"{self.profile.name} 连接中断，正在重连")
        if not self._recycle_agent():
            return False
        self._set_state(THINKING, f"{self.profile.name} 已重连，继续这一轮")
        self._run_prompt(text, allow_recycle=False)
        return True

    def _run_prompt(self, text: str, *, allow_recycle: bool = True) -> None:
        assert self.client and self.session_id
        t0 = time.monotonic()
        handed_off = False
        try:
            result = self.client.prompt(self.session_id, text)
            final = "".join(self._stream_text).strip()
            refusal = prompt_refusal_text(result) if not final else None
            if allow_recycle and transport_dead_text(final or refusal or ""):
                log.warning("agent transport died, recycling once")
                handed_off = self._recover_transport(text)
                if handed_off:
                    return
                final = final or "连接中断后重连失败"
            if refusal and auth_required_text(refusal):
                _clear_workbuddy_signed_in()
                self._open_login(self.client, self.profile, self.auth_methods, None)
                _mark_workbuddy_signed_in()
                self._stream_text = []
                self._assistant_streamed = False
                self._set_state(THINKING, f"已登录，继续等待 {self.profile.name}")
                result = self.client.prompt(self.session_id, text)
                final = "".join(self._stream_text).strip()
                refusal = prompt_refusal_text(result) if not final else None
            if final and not self._assistant_streamed:
                with self._lock:
                    self._msg_seq += 1
                    aid = f"a{self._msg_seq}"
                    self.transcript.append({"id": aid, "role": "assistant",
                                            "text": final, "ts": time.time()})
            with self._lock:
                self.metrics["turns"] += 1
                self.metrics["lastFirstTokenMs"] = (
                    round((self._first_token_at - self._turn_t0) * 1000)
                    if self._first_token_at and self._turn_t0 else None
                )
                self.metrics["lastTurnMs"] = round((time.monotonic() - t0) * 1000)
            if refusal:
                with self._lock:
                    self.last_error = refusal
                detail = "需要先登录" if auth_required_text(refusal) else "指令被拒绝"
                self._set_state(ERROR, detail)
                self._remember_assistant(refusal)
                self.publish({"type": "error", "message": refusal})
                return
            save_id = None
            with self._lock:
                save_id = self._meeting_save_id
                self._meeting_save_id = None
            if save_id and final:
                self._write_meeting(save_id, [], final)
            self._remember_assistant(final, meeting_id=save_id)
            self._set_state(DONE, "本轮完成")
            self.publish({
                "type": "done",
                "stopReason": result.get("stopReason"),
                "durationMs": round((time.monotonic() - t0) * 1000),
                "firstTokenMs": self.metrics["lastFirstTokenMs"],
                "text": final,
            })
        except Exception as exc:
            if allow_recycle and transport_dead_text(str(exc)):
                log.warning("agent transport exception, recycling once: %s", exc)
                handed_off = self._recover_transport(text)
                if handed_off:
                    return
            with self._lock:
                self.last_error = str(exc)
                self._meeting_save_id = None
            self._set_state(ERROR, str(exc))
            self._remember_assistant(str(exc))
            self.publish({"type": "error", "message": str(exc)})
        finally:
            if not handed_off:
                with self._lock:
                    self.busy = False

    def resolve_approval(self, approval_id: str, decision: str, *, remote: bool = False) -> tuple[bool, str | None]:
        with self._lock:
            record = self.pending.get(approval_id)
        if not record:
            return False, "审批已失效"
        # A record created while the face was not allowed to approve stays blocked.
        if decision == "allow" and remote and not record["canApprove"]:
            return False, "这项操作不能在表盘上批准"
        option_id = _pick_option(record["options"], decision)
        if not self.client:
            return False, f"{self.profile.name} 尚未连接"
        self.client.respond_permission(record["requestId"], option_id)
        with self._lock:
            self.pending.pop(approval_id, None)
            self.metrics["approvals"] += 1
            self.metrics["lastApprovalMs"] = round((time.time() - record["createdAt"]) * 1000)
        self.publish({"type": "approval_resolved", "id": approval_id, "decision": decision,
                      "latencyMs": self.metrics["lastApprovalMs"]})
        self._set_state(RUNNING_TOOL, "已批准，继续执行" if decision == "allow" else "已拒绝")
        return True, None

    def set_approval_policy(self, raw: dict[str, Any]) -> dict[str, bool]:
        with self._lock:
            policy = dict(self.approval_policy)
            for level in _POLICY_LEVELS:
                if isinstance(raw, dict) and level in raw:
                    policy[level] = _policy_bool(raw[level])
            self.approval_policy = policy
            saved = dict(policy)
        try:
            save_approval_policy(saved)
        except OSError:
            log.exception("approval policy save failed")
        log.info("approval policy %s", saved)
        self.publish({"type": "approval_policy", "policy": saved})
        return saved

    def cancel(self) -> tuple[bool, str | None]:
        if not self.client or not self.session_id:
            return False, f"{self.profile.name} 尚未连接"
        self.client.cancel(self.session_id)
        for aid, record in list(self.pending.items()):
            self.client.respond_permission(record["requestId"], None)
            self.pending.pop(aid, None)
        self._set_state(IDLE, "已取消")
        self.publish({"type": "cancelled"})
        return True, None

    def push_toast(
        self,
        title: str,
        body: str = "",
        *,
        level: str = "info",
        ttl_ms: int = 6000,
        speak: bool = False,
    ) -> dict[str, Any]:
        raw_title = (title or "").strip()
        raw_body = (body or "").strip()
        spoken = raw_body or raw_title
        if raw_title and raw_body and raw_body not in raw_title:
            spoken = f"{raw_title}。{raw_body}"
        title = raw_title[:40]
        body = raw_body[:280]
        level = level if level in TOAST_LEVELS else "info"
        try:
            ttl_ms = int(ttl_ms)
        except (TypeError, ValueError):
            ttl_ms = 6000
        ttl_ms = max(1500, min(60_000, ttl_ms))
        payload = {
            "type": "toast",
            "title": title,
            "body": body,
            "level": level,
            "ttlMs": ttl_ms,
            "speak": bool(speak),
            "speakText": spoken[:2000] if speak else "",
        }
        self.publish(payload)
        return payload

    def ring_device(self, pattern: str = "short", repeat: int = 1) -> None:
        pat = pattern if pattern in ("short", "long") else "short"
        rep = max(1, min(3, int(repeat or 1)))
        self.publish({"type": "ring_device", "pattern": pat, "repeat": rep})

    def ping_pc_from_device(self) -> None:
        self.publish({"type": "ping_pc", "source": "device"})

    def run_macro(self, macro_id: str) -> tuple[bool, str | None]:
        mid = (macro_id or "").strip()
        if not mid:
            return False, "宏 id 为空"
        spec = load_macros().get(mid)
        if not spec:
            return False, "未知宏"
        threading.Thread(target=_run_macro_worker, args=(mid, spec), daemon=True).start()
        return True, None


bridge = Bridge()
app = FastAPI(title="Pager G1 Bridge")
app.mount("/static", StaticFiles(directory=str(STATIC)), name="static")


def _client_ip(request: Request | WebSocket) -> str:
    client = request.client
    return client.host if client else ""


def _is_local(request: Request) -> bool:
    host = _client_ip(request)
    return host in {"127.0.0.1", "::1", "::ffff:127.0.0.1", "localhost"}


def _admin_forbidden() -> JSONResponse:
    return JSONResponse(
        status_code=403,
        content={"ok": False, "error": "localhost_only", "message": "仅允许本机管理接口"},
    )


def _pairing_error(exc: PairingError) -> JSONResponse:
    return JSONResponse(
        status_code=exc.status_code,
        content={"ok": False, "error": exc.code, "message": exc.message},
        headers={"Cache-Control": "no-store"},
    )


def _device_fingerprint() -> str:
    return tlsutil.read_fingerprint(CERT_DIR / tlsutil.CERT_NAME)


def _mdns_name() -> str:
    return mdns_instance_name()


def device_info_payload() -> dict[str, Any]:
    fp = _device_fingerprint()
    secure = os.environ.get("PAGER_INSECURE_DEVICE", "") not in ("1", "true", "TRUE", "yes", "on")
    return {
        "secure": secure,
        "tlsPort": DEVICE_PORT,
        "wsPath": DEVICE_WS_PATH,
        "pairPath": DEVICE_PAIR_PATH,
        "fingerprint": fp,
        "fingerprintDisplay": tlsutil.format_fingerprint(fp),
        "mdnsService": "_pager._tcp.local.",
        "mdnsName": _mdns_name(),
        "activeProfile": bridge.active_profile_id,
    }


@app.on_event("startup")
async def _startup() -> None:
    bridge.attach_loop(asyncio.get_running_loop())
    bridge.start_async()
    bridge.start_host_sampler()


@app.on_event("shutdown")
async def _shutdown() -> None:
    bridge.stop_host_sampler()
    if bridge.client:
        bridge.client.close(timeout=5)


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(str(STATIC / "index.html"))


@app.get("/health")
async def health() -> dict[str, Any]:
    return {
        "ok": True,
        "state": bridge.state,
        "sessionId": bridge.session_id,
        "profile": bridge.active_profile_id,
    }


@app.get("/api/state")
async def state() -> dict[str, Any]:
    return bridge.snapshot()


@app.get("/api/session/history")
async def api_session_history(request: Request, limit: int = 200) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    return JSONResponse({"ok": True, "items": read_session_history(limit)})


@app.get("/api/meetings")
async def api_meetings(request: Request) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    return JSONResponse({"ok": True, "meetings": list_meetings_index()})


@app.get("/api/meetings/{meeting_id}")
async def api_meeting_detail(request: Request, meeting_id: str) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    record = load_meeting_record(meeting_id)
    if not record:
        return JSONResponse(status_code=404, content={"ok": False, "error": "not_found"})
    return JSONResponse({"ok": True, "meeting": record})


@app.post("/api/prompt")
async def api_prompt(payload: dict[str, Any]) -> JSONResponse:
    ok, err = bridge.submit_prompt(payload.get("text", ""))
    return JSONResponse(status_code=200 if ok else 409, content={"ok": ok, "error": err})


@app.post("/api/approval")
async def api_approval(payload: dict[str, Any]) -> JSONResponse:
    ok, err = bridge.resolve_approval(payload.get("id", ""), payload.get("decision", "deny"))
    return JSONResponse(status_code=200 if ok else 400, content={"ok": ok, "error": err})


@app.post("/api/cancel")
async def api_cancel() -> JSONResponse:
    ok, err = bridge.cancel()
    return JSONResponse(status_code=200 if ok else 409, content={"ok": ok, "error": err})


@app.post("/api/retry")
async def api_retry() -> dict[str, Any]:
    bridge.retry()
    return {"ok": True}


@app.post("/api/notify")
async def api_notify(request: Request, payload: dict[str, Any]) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    ip = _client_ip(request)
    if not _notify_rate.allow(ip):
        return JSONResponse(
            status_code=429,
            content={"ok": False, "error": "rate_limited", "message": "通知过于频繁"},
        )
    title = str(payload.get("title") or "")
    body = str(payload.get("body") or payload.get("message") or "")
    level = str(payload.get("level") or "info")
    ttl_ms = payload.get("ttlMs", payload.get("ttl_ms", 6000))
    speak = bool(payload.get("speak"))
    if not title and not body:
        return JSONResponse(
            status_code=400,
            content={"ok": False, "error": "empty", "message": "title 或 body 至少填一项"},
        )
    event = bridge.push_toast(title or "通知", body, level=level, ttl_ms=ttl_ms, speak=speak)
    PAIRING.audit(
        "notify.push",
        ip=ip,
        level=event.get("level"),
        title_len=len(event.get("title") or ""),
        body_len=len(event.get("body") or ""),
    )
    return JSONResponse({"ok": True, "event": event})


@app.post("/api/find-device")
async def api_find_device(request: Request, payload: dict[str, Any] | None = None) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    ip = _client_ip(request)
    if not _ring_rate.allow(ip):
        return JSONResponse(
            status_code=429,
            content={"ok": False, "error": "rate_limited", "message": "寻呼过于频繁"},
        )
    body = payload or {}
    if not DEVICE_HUB.connected_ids():
        return JSONResponse(
            status_code=409,
            content={"ok": False, "error": "offline", "message": "没有已连接的设备"},
        )
    pattern = str(body.get("pattern") or "short")
    repeat = body.get("repeat", 1)
    bridge.ring_device(pattern, int(repeat or 1))
    PAIRING.audit("ring.device", ip=ip, pattern=pattern, repeat=repeat)
    return JSONResponse({"ok": True})


@app.get("/api/macros")
async def api_macros(request: Request) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    return JSONResponse({"ok": True, "macros": public_macros()})


# ---------------------------------------------------------------------------
# G1 admin surface: localhost only
# ---------------------------------------------------------------------------

@app.get("/api/profiles")
async def api_profiles(request: Request) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    return JSONResponse({
        "ok": True,
        "active": bridge.active_profile_id,
        "default": bridge.registry.default_id,
        "source": str(bridge.registry.source) if bridge.registry.source else None,
        "profileError": bridge.profile_error,
        "profiles": bridge.registry.public_list(),
    })


@app.post("/api/profile")
async def api_profile(request: Request, payload: dict[str, Any]) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    ok, err = bridge.select_profile(str(payload.get("id") or ""))
    return JSONResponse(status_code=200 if ok else 409, content={"ok": ok, "error": err})


@app.get("/api/device/info")
async def api_device_info(request: Request) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    info = device_info_payload()
    info["devices"] = len(PAIRING.public_devices())
    info["connected"] = len(DEVICE_HUB.connected_ids())
    return JSONResponse({"ok": True, **info})


@app.post("/device/pair/start")
async def device_pair_start(request: Request, payload: dict[str, Any] | None = None) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    body = payload or {}
    try:
        session = PAIRING.start(name_hint=str(body.get("name") or ""), ip=_client_ip(request))
    except PairingError as exc:
        return _pairing_error(exc)
    info = device_info_payload()
    return JSONResponse(
        {
            "ok": True,
            "code": session.code,
            "pairingId": session.id,
            "expiresIn": max(0, int(round(session.expires_at - time.time()))),
            "expiresAt": session.expires_at,
            **info,
        },
        headers={"Cache-Control": "no-store"},
    )


@app.get("/device/list")
async def device_list(request: Request) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    devices = PAIRING.public_devices(DEVICE_HUB.connected_ids())
    return JSONResponse({"ok": True, "devices": devices, "count": len(devices)})


@app.post("/device/revoke")
async def device_revoke(request: Request, payload: dict[str, Any]) -> JSONResponse:
    if not _is_local(request):
        return _admin_forbidden()
    device_id = str(payload.get("id") or payload.get("deviceId") or "")
    record = PAIRING.revoke(device_id, ip=_client_ip(request))
    if not record:
        return JSONResponse(status_code=404, content={"ok": False, "error": "not_found"})
    await DEVICE_HUB.close_device(device_id)
    return JSONResponse({"ok": True, "device": record.public()})


# ---------------------------------------------------------------------------
# Shared WebSocket session: PC UI (localhost) and authenticated devices
# ---------------------------------------------------------------------------

async def serve_bridge_ws(ws: WebSocket, *, subprotocol: str | None = None, remote: bool = False) -> None:
    await ws.accept(subprotocol=subprotocol)
    q = bridge.subscribe()
    await ws.send_json(bridge.snapshot(for_device=remote))
    session = {"speak": False, "voice": bridge.watch_voice}
    send_lock = asyncio.Lock()
    speak_q: asyncio.Queue[tuple[str, str, int]] = asyncio.Queue()
    speak_gen = {"n": 0}
    tail = {"buf": ""}

    def cut_speech() -> None:
        speak_gen["n"] += 1
        tail["buf"] = ""
        while True:
            try:
                speak_q.get_nowait()
            except asyncio.QueueEmpty:
                break
        speak_q.put_nowait(("cut", "", speak_gen["n"]))

    def enqueue_speech(text: str, *, end: bool = False) -> None:
        if not session["speak"]:
            return
        gen = speak_gen["n"]
        for piece in speech_segments(text):
            speak_q.put_nowait(("say", piece, gen))
        if end:
            speak_q.put_nowait(("end", "", gen))

    async def speaker() -> None:
        utt_seq = 1
        utt_open = False
        started: float | None = None
        total = 0
        while True:
            kind, text, gen = await speak_q.get()
            if gen != speak_gen["n"] and kind != "cut":
                continue
            if kind == "cut":
                if utt_open:
                    async with send_lock:
                        await ws.send_json({
                            "type": "audio_out", "seq": 1, "rate": 16000, "last": True, "b64": "",
                        })
                utt_seq = 1
                utt_open = False
                started = None
                total = 0
                continue
            if kind == "end":
                if utt_open:
                    async with send_lock:
                        await ws.send_json({
                            "type": "audio_out", "seq": utt_seq, "rate": 16000, "last": True, "b64": "",
                        })
                    log.info("spoke %d bytes", total)
                utt_seq = 1
                utt_open = False
                started = None
                total = 0
                continue
            with bridge._lock:
                voice = normalize_voice(bridge.watch_voice or session.get("voice"))
            session["voice"] = voice
            try:
                pcm = await asyncio.to_thread(
                    synthesize_segment,
                    text,
                    fade_in=not utt_open,
                    fade_out=True,
                    voice=voice,
                )
            except Exception:
                log.exception("speak segment failed")
                continue
            if gen != speak_gen["n"] or not pcm or not session["speak"]:
                continue
            step = 1800
            for off in range(0, len(pcm), step):
                if gen != speak_gen["n"]:
                    break
                chunk = pcm[off:off + step]
                async with send_lock:
                    await ws.send_json({
                        "type": "audio_out",
                        "seq": utt_seq,
                        "rate": 16000,
                        "last": False,
                        "b64": base64.b64encode(chunk).decode("ascii"),
                    })
                utt_seq += 1
                utt_open = True
                total += len(chunk)
                if started is None:
                    started = time.monotonic()
                ahead = total / 32000 - (time.monotonic() - started)
                if ahead > 3.0:
                    await asyncio.sleep(ahead - 2.0)

    async def sender() -> None:
        while True:
            event = await q.get()
            # Usage/metrics/thought deltas are web-only diagnostics. Host
            # samples ARE wanted by the watch: its 资源 page draws the same
            # CPU/RAM/GPU/net rings as this client.
            if remote and event.get("type") in {"usage", "metrics", "thought_delta"}:
                continue
            async with send_lock:
                await ws.send_json(event)
            kind = event.get("type")
            if kind in {"user", "cancelled", "profile"}:
                cut_speech()
            elif session["speak"] and kind == "assistant_delta":
                tail["buf"] += str(event.get("text") or "")
                ready, tail["buf"] = take_sentences(tail["buf"])
                for piece in ready:
                    enqueue_speech(piece)
            elif session["speak"] and kind == "done":
                ready, rest = take_sentences(tail["buf"])
                tail["buf"] = ""
                if rest.strip():
                    ready.append(rest.strip())
                for piece in ready:
                    enqueue_speech(piece)
                enqueue_speech("", end=True)
            elif session["speak"] and kind == "toast" and event.get("speak"):
                with bridge._lock:
                    busy = bridge.busy
                if not busy:
                    text = str(event.get("speakText") or event.get("body") or event.get("title") or "").strip()
                    if text:
                        cut_speech()
                        enqueue_speech(text, end=True)

    sender_task = asyncio.create_task(sender())
    speaker_task = asyncio.create_task(speaker())
    audio = bytearray()
    heard_parts: list[str] = []
    try:
        while True:
            msg = await ws.receive_json()
            kind = msg.get("type")
            if kind == "prompt":
                ok, err = bridge.submit_prompt(msg.get("text", ""))
                if not ok:
                    await ws.send_json({"type": "error", "message": err})
            elif kind == "audio":
                log.info("audio in seq=%s last=%s", msg.get("seq"), msg.get("last"))
                try:
                    seq = int(msg.get("seq") or 0)
                    if seq <= 1:
                        audio = bytearray()
                    chunk = base64.b64decode(msg.get("b64") or "", validate=False)
                    # 15 s of 16 kHz mono, plus one chunk of slack.
                    if len(audio) + len(chunk) <= 16000 * 2 * 16:
                        audio.extend(chunk)
                    if msg.get("last"):
                        pcm = bytes(audio)
                        audio = bytearray()
                        rate = int(msg.get("rate") or 16000)
                        more = bool(msg.get("more"))
                        text = ""
                        err = None
                        if len(pcm) >= rate // 2:
                            text, err = await asyncio.to_thread(transcribe_pcm, pcm, rate)
                        if text:
                            heard_parts.append(text)
                            log.info("segment %d chars more=%s", len(text), more)
                        with bridge._lock:
                            meeting_phase = bridge.meeting.get("phase")
                            meeting_id = bridge.meeting.get("id")
                        if meeting_phase in {"recording", "summarizing"}:
                            heard_parts.clear()
                            if text:
                                joined_part = polish_utterance(text)
                                if joined_part:
                                    bridge.note_meeting(joined_part)
                            if not more and meeting_phase == "summarizing" and meeting_id:
                                threading.Thread(
                                    target=bridge._finish_meeting,
                                    args=(str(meeting_id),),
                                    daemon=True,
                                ).start()
                            continue
                        if more:
                            continue
                        joined = polish_utterance("".join(heard_parts))
                        heard_parts.clear()
                        if joined:
                            await ws.send_json({"type": "heard", "text": joined})
                            ok, perr = bridge.submit_prompt(joined)
                            if not ok:
                                await ws.send_json({"type": "error", "message": perr or "没送出去"})
                        else:
                            note = err or "没听清"
                            await ws.send_json({"type": "error", "message": note})
                            enqueue_speech(note, end=True)
                except Exception:
                    log.exception("audio prompt failed")
                    audio = bytearray()
                    heard_parts.clear()
                    await ws.send_json({"type": "error", "message": "没听清"})
                    enqueue_speech("没听清", end=True)
            elif kind == "approval_policy":
                if remote:
                    await ws.send_json({"type": "error", "message": "自动审批只能在电脑上设置"})
                else:
                    bridge.set_approval_policy(msg if isinstance(msg, dict) else {})
            elif kind == "approval":
                ok, err = bridge.resolve_approval(
                    msg.get("id", ""), msg.get("decision", "deny"), remote=remote
                )
                if not ok:
                    await ws.send_json({"type": "error", "message": err})
            elif kind == "meeting":
                action = str(msg.get("action") or "")
                if action == "stop" or msg.get("on") is False:
                    ok, err = bridge.stop_meeting()
                else:
                    if not DEVICE_HUB.connected_ids():
                        ok, err = False, "手表还没连上，会议记录要用手表上的麦克风"
                    else:
                        ok, err = bridge.start_meeting()
                if not ok:
                    await ws.send_json({"type": "error", "message": err})
            elif kind == "cancel":
                ok, err = bridge.cancel()
                if not ok:
                    await ws.send_json({"type": "error", "message": err})
            elif kind == "prefs":
                session["speak"] = bool(msg.get("speak"))
                voice = normalize_voice(str(msg.get("voice") or session.get("voice") or ""))
                session["voice"] = voice
                log.info("device announce %s voice %s", "on" if session["speak"] else "off", voice)
                changed = False
                with bridge._lock:
                    if bridge.watch_voice != voice:
                        bridge.watch_voice = voice
                        changed = True
                if changed:
                    bridge.publish({"type": "voice", "value": voice})
                if msg.get("volume") is not None:
                    try:
                        vol = max(0, min(100, int(msg.get("volume"))))
                    except (TypeError, ValueError):
                        vol = None
                    if vol is not None:
                        with bridge._lock:
                            bridge.watch_volume = vol
                        bridge.publish({"type": "volume", "value": vol})
            elif kind == "voice":
                if remote:
                    await ws.send_json({"type": "error", "message": "声线由手表设置"})
                else:
                    voice = normalize_voice(str(msg.get("value") or ""))
                    with bridge._lock:
                        bridge.watch_voice = voice
                    bridge.publish({"type": "voice", "value": voice})
            elif kind == "volume":
                try:
                    vol = max(0, min(100, int(msg.get("value"))))
                except (TypeError, ValueError):
                    vol = None
                if vol is not None:
                    with bridge._lock:
                        bridge.watch_volume = vol
                    bridge.publish({"type": "volume", "value": vol})
            elif kind == "ping":
                await ws.send_json({"type": "pong"})
            elif kind == "ping_pc":
                if not remote:
                    await ws.send_json({"type": "error", "message": "ping_pc 仅设备可用"})
                else:
                    bridge.ping_pc_from_device()
                    PAIRING.audit("ring.pc", ip=_client_ip(ws))
            elif kind == "macro":
                if not remote:
                    await ws.send_json({"type": "error", "message": "macro 仅设备可用"})
                else:
                    ok, err = bridge.run_macro(str(msg.get("id") or ""))
                    if not ok:
                        await ws.send_json({"type": "error", "message": err})
    except WebSocketDisconnect:
        pass
    except Exception:
        log.exception("device session failed")
    finally:
        speaker_task.cancel()
        sender_task.cancel()
        bridge.unsubscribe(q)


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    await serve_bridge_ws(ws)


def main() -> None:
    import uvicorn
    uvicorn.run(
        app,
        host="127.0.0.1",
        port=int(os.environ.get("PAGER_PORT", "8765")),
        log_level="info",
    )


if __name__ == "__main__":
    main()
