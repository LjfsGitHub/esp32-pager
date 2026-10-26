"""Drive the Pager G0 bridge from the command line.

Examples:
    python pager_bridge/drive.py "列出当前目录的文件" --approve none
    python pager_bridge/drive.py "创建 demo.txt" --approve allow
    python pager_bridge/drive.py "删除 demo.txt" --approve deny
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

BASE = "http://127.0.0.1:8765"
HERE = Path(__file__).resolve().parent


def req(method: str, path: str, payload: dict | None = None) -> dict:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
    r = urllib.request.Request(
        BASE + path, data=data, method=method,
        headers={"Content-Type": "application/json; charset=utf-8"},
    )
    try:
        with urllib.request.urlopen(r, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        try:
            return json.loads(body)
        except json.JSONDecodeError:
            return {"ok": False, "error": f"HTTP {e.code}: {body[:200]}"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("prompt")
    ap.add_argument("--approve", choices=["allow", "deny", "none"], default="none")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--out", default=str(HERE / "last_run.json"))
    args = ap.parse_args()

    state = req("GET", "/api/state")
    if state["state"] not in ("IDLE", "DONE", "ERROR"):
        print(f"bridge not ready: {state['state']} {state.get('stateDetail')}")
        return 2

    t0 = time.monotonic()
    resp = req("POST", "/api/prompt", {"text": args.prompt})
    if not resp.get("ok"):
        print(f"prompt rejected: {resp.get('error')}")
        return 2

    seen_tools: set[str] = set()
    handled: set[str] = set()
    first_event = None
    approvals = []
    deadline = t0 + args.timeout
    last_state = None

    while time.monotonic() < deadline:
        s = req("GET", "/api/state")
        st = s["state"]
        if first_event is None and (s["transcript"] or s["tools"] or s["pending"]):
            first_event = time.monotonic() - t0
        if st != last_state:
            print(f"[{time.monotonic()-t0:6.1f}s] state={st} detail={s.get('stateDetail','')}")
            last_state = st
        for tool in s["tools"]:
            if tool["id"] not in seen_tools:
                seen_tools.add(tool["id"])
                title = (tool.get("title") or "")[:100]
                print(f"           tool: {title} [{tool.get('status')}]")
        for a in s["pending"]:
            if a["id"] in handled:
                continue
            handled.add(a["id"])
            approvals.append(a)
            print(f"           approval: {a['risk']} {a.get('command','')[:120]}")
            if args.approve != "none":
                decision = args.approve
                r = req("POST", "/api/approval", {"id": a["id"], "decision": decision})
                print(f"           -> {decision}: {r}")
        if st in ("DONE", "ERROR") and not s["pending"]:
            break
        time.sleep(0.4)

    s = req("GET", "/api/state")
    report = {
        "prompt": args.prompt,
        "approve_mode": args.approve,
        "elapsed_s": round(time.monotonic() - t0, 2),
        "first_event_s": round(first_event, 2) if first_event is not None else None,
        "state": s["state"],
        "stateDetail": s.get("stateDetail"),
        "metrics": s.get("metrics"),
        "approvals": approvals,
        "tools": s.get("tools"),
        "transcript": s.get("transcript"),
    }
    Path(args.out).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    final = ""
    for m in reversed(s.get("transcript") or []):
        if m.get("role") == "assistant":
            final = m.get("text") or ""
            break
    print(f"\nstate={s['state']} elapsed={report['elapsed_s']}s approvals={len(approvals)}")
    print("final (first 400 chars):")
    print(final[:400])
    print(f"\nfull report -> {args.out}")
    return 0 if s["state"] != "ERROR" else 1


if __name__ == "__main__":
    sys.exit(main())
