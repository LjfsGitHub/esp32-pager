"""Minimal ACP probe: spawn Hermes, create a session, send one prompt, dump raw frames."""
from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import threading
import time

HERMES_ACP = os.path.expandvars(r"%LOCALAPPDATA%\hermes\bin\hermes-acp.exe")
WORKSPACE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "pager_workspace")


class Client:
    def __init__(self, proc):
        self.proc = proc
        self.next_id = 0
        self.pending: dict[int, queue.Queue] = {}
        self.lock = threading.Lock()
        self.frames = []
        threading.Thread(target=self._pump, daemon=True).start()

    @staticmethod
    def _decode(raw: bytes) -> str:
        raw = raw.strip()
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            return raw.decode("gbk", errors="replace")

    def _pump(self):
        for raw in self.proc.stdout:
            line = self._decode(raw)
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                print(f"[stray stdout] {line[:400]}", flush=True)
                continue
            self.frames.append(msg)
            if "method" in msg and "id" in msg:
                # server -> client request; auto-answer permissions for the probe
                method = msg["method"]
                print(f"[server request] {method}: {json.dumps(msg.get('params'), ensure_ascii=False)[:1200]}", flush=True)
                if method == "session/request_permission":
                    options = (msg.get("params") or {}).get("options") or []
                    pick = next((o for o in options if o.get("kind") == "allow_once"), options[0] if options else None)
                    result = ({"outcome": {"outcome": "selected", "optionId": pick["optionId"]}}
                              if pick else {"outcome": {"outcome": "cancelled"}})
                else:
                    result = None
                self._send({"jsonrpc": "2.0", "id": msg["id"], "result": result})
                continue
            if "method" in msg:
                print(f"[notify] {msg['method']}: {json.dumps(msg.get('params'), ensure_ascii=False)[:1200]}", flush=True)
                continue
            q = self.pending.pop(msg.get("id"), None)
            if q:
                q.put(msg)

    def _send(self, msg):
        # Pure-ASCII JSON: safe regardless of the child's stdio encoding.
        data = (json.dumps(msg, ensure_ascii=True) + "\n").encode("ascii")
        with self.lock:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()

    def request(self, method, params, timeout=300):
        self.next_id += 1
        rid = self.next_id
        q = queue.Queue()
        self.pending[rid] = q
        t0 = time.monotonic()
        self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        msg = q.get(timeout=timeout)
        dt = time.monotonic() - t0
        print(f"[rpc] {method} -> {dt:.1f}s: {json.dumps(msg, ensure_ascii=False)[:800]}", flush=True)
        if "error" in msg:
            raise RuntimeError(msg["error"])
        return msg.get("result") or {}


def main():
    os.makedirs(WORKSPACE, exist_ok=True)
    env = dict(os.environ)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    stderr = open(os.path.join(os.path.dirname(__file__), "acp_stderr.log"), "w", encoding="utf-8")
    proc = subprocess.Popen(
        [HERMES_ACP], cwd=WORKSPACE, env=env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
        bufsize=0,
    )
    client = Client(proc)
    try:
        client.request("initialize", {
            "protocolVersion": 1,
            "clientCapabilities": {"fs": {"readTextFile": False, "writeTextFile": False}, "terminal": False},
            "clientInfo": {"name": "pager-g0-probe", "version": "0.1"},
        }, timeout=60)
        session = client.request("session/new", {"cwd": WORKSPACE, "mcpServers": []}, timeout=240)
        sid = session.get("sessionId")
        print(f"[session] {sid}", flush=True)
        result = client.request("session/prompt", {
            "sessionId": sid,
            "prompt": [{"type": "text", "text": "只回复两个字：收到。不要调用任何工具。"}],
        }, timeout=300)
        print(f"[prompt result] {json.dumps(result, ensure_ascii=False)}", flush=True)
        out = os.path.join(os.path.dirname(__file__), "probe_frames.jsonl")
        with open(out, "w", encoding="utf-8") as f:
            for frame in client.frames:
                f.write(json.dumps(frame, ensure_ascii=False) + "\n")
        chunks = []
        for frame in client.frames:
            upd = ((frame.get("params") or {}).get("update") or {})
            if upd.get("sessionUpdate") == "agent_message_chunk" and (upd.get("content") or {}).get("type") == "text":
                chunks.append(upd["content"]["text"])
        final = "".join(chunks)
        with open(os.path.join(os.path.dirname(__file__), "probe_final.txt"), "w", encoding="utf-8") as f:
            f.write(final)
        print(f"[final repr] {final!r}", flush=True)
    finally:
        try:
            proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
        stderr.close()


if __name__ == "__main__":
    main()
