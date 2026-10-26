#!/usr/bin/env python3
"""
Pager QA guardian: periodic or continuous runs of run.py; auto-bootstrap golden on first pass;
writes guardian_state.json for external schedulers (Task Scheduler, /loop, CI).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
STATE_PATH = ROOT / "guardian_state.json"
RUN_PY = ROOT / "run.py"


def load_state() -> dict:
    if STATE_PATH.is_file():
        return json.loads(STATE_PATH.read_text(encoding="utf-8"))
    return {
        "runs": 0,
        "passes": 0,
        "failures": 0,
        "consecutive_failures": 0,
        "last_ok": None,
        "last_report": None,
    }


def save_state(st: dict) -> None:
    STATE_PATH.write_text(json.dumps(st, indent=2), encoding="utf-8")


def golden_incomplete() -> bool:
    p = ROOT / "golden" / "480" / "scenes.json"
    if not p.is_file():
        return True
    data = json.loads(p.read_text(encoding="utf-8"))
    fps = data.get("fingerprints") or {}
    scenes = data.get("scenes") or []
    return len(fps) < len(scenes)


def run_once(port: str, monitor_sec: float, bootstrap: bool, skip_build: bool, skip_flash: bool) -> tuple[int, str | None]:
    cmd = [
        sys.executable,
        str(RUN_PY),
        "--port",
        port,
        "--monitor-sec",
        str(monitor_sec),
    ]
    if bootstrap:
        cmd.append("--bootstrap-golden")
    if skip_build:
        cmd.append("--skip-build")
    if skip_flash:
        cmd.append("--skip-flash")
    print(f"[guardian] {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=str(ROOT))
    report = latest_report()
    return proc.returncode, report


def latest_report() -> str | None:
    reports = sorted(ROOT.glob("reports/report_*.json"), key=lambda p: p.stat().st_mtime, reverse=True)
    return str(reports[0]) if reports else None


def main() -> int:
    ap = argparse.ArgumentParser(description="Pager QA guardian daemon")
    ap.add_argument("--port", default="COM7")
    ap.add_argument(
        "--interval-sec",
        type=float,
        default=1800.0,
        help="Sleep between runs; 0 = immediate retry (continuous mode)",
    )
    ap.add_argument(
        "--until-pass",
        action="store_true",
        help="Keep looping until run.py exits 0, then stop",
    )
    ap.add_argument("--monitor-sec", type=float, default=90.0)
    ap.add_argument("--once", action="store_true", help="Single run then exit")
    ap.add_argument("--daemon", action="store_true", help="Loop forever with interval")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-flash", action="store_true")
    ap.add_argument("--always-bootstrap", action="store_true")
    args = ap.parse_args()

    if not args.once and not args.daemon:
        args.once = True

    st = load_state()

    def tick() -> int:
        bootstrap = args.always_bootstrap or golden_incomplete()
        if bootstrap:
            print("[guardian] golden incomplete → bootstrap mode")
        code, report = run_once(
            args.port,
            args.monitor_sec,
            bootstrap=bootstrap,
            skip_build=args.skip_build,
            skip_flash=args.skip_flash,
        )
        st["runs"] = st.get("runs", 0) + 1
        st["last_run"] = datetime.now(timezone.utc).isoformat()
        st["last_report"] = report
        st["last_exit_code"] = code
        if code == 0:
            st["passes"] = st.get("passes", 0) + 1
            st["consecutive_failures"] = 0
            st["last_ok"] = st["last_run"]
        else:
            st["failures"] = st.get("failures", 0) + 1
            st["consecutive_failures"] = st.get("consecutive_failures", 0) + 1
            print(f"[guardian] run failed (exit {code}); retrying…")
        save_state(st)
        return code

    if args.once and not args.until_pass:
        return tick()

    if args.until_pass:
        print(f"[guardian] continuous until PASS port={args.port} interval={args.interval_sec}s")
        while True:
            code = tick()
            if code == 0:
                print("[guardian] QA PASS — done")
                return 0
            if args.interval_sec <= 0:
                time.sleep(8.0)
            else:
                time.sleep(max(60.0, args.interval_sec))
        return 1

    print(f"[guardian] daemon started interval={args.interval_sec}s port={args.port}")
    while True:
        tick()
        if args.interval_sec <= 0:
            time.sleep(8.0)
        else:
            time.sleep(max(60.0, args.interval_sec))


if __name__ == "__main__":
    sys.exit(main())
