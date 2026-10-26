#!/usr/bin/env python3
"""
Pager QA orchestrator (2.16 / COM7): build QA firmware, flash, capture UI fingerprints, check logs.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

try:
    import serial
except ImportError:
    serial = None

ROOT = Path(__file__).resolve().parent
PROJECT = Path(r"D:\pager_firmware_2_16\examples\esp-idf\03_esp-brookesia")
GOLDEN = ROOT / "golden" / "480"
REPORTS = ROOT / "reports"
IDF_EXPORT = r"D:\ESP32\.tools\esp-idf-v5.5.5\export.bat"

BAD_LOG = re.compile(
    r"Guru Meditation|task_wdt|LoadProhibited|panic'ed|QA: startApp failed",
    re.I,
)
UI_WARN_LOG = re.compile(
    r"display lock timeout|ESP_ERR_NO_MEM|Draw bitmap failed",
    re.I,
)
SOFTAP_RE = re.compile(r"softap\s+(Pager-[0-9A-Fa-f]{4})", re.I)
GOT_IP_RE = re.compile(r"got ip (\d+\.\d+\.\d+\.\d+)", re.I)
QA_HTTP_RE = re.compile(r"qa http (\d+\.\d+\.\d+\.\d+)", re.I)


@dataclass
class Result:
    ok: bool = True
    steps: list[dict] = field(default_factory=list)
    log_hits: list[str] = field(default_factory=list)

    def fail(self, name: str, detail: str) -> None:
        self.ok = False
        self.steps.append({"step": name, "ok": False, "detail": detail})

    def pass_(self, name: str, detail: str = "") -> None:
        self.steps.append({"step": name, "ok": True, "detail": detail})


def run_cmd(cmd: list[str], cwd: Path, timeout: int = 600) -> subprocess.CompletedProcess:
    print(f"[cmd] {' '.join(cmd)}")
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        timeout=timeout,
        shell=False,
    )


def ensure_qa_sdkconfig(project: Path) -> None:
    """Enable CONFIG_PAGER_QA in sdkconfig (-DCONFIG_* does not apply to Kconfig)."""
    sdk = project / "sdkconfig"
    qa_on = "CONFIG_PAGER_QA=y"
    qa_off = "# CONFIG_PAGER_QA is not set"
    text = sdk.read_text(encoding="utf-8", errors="replace")
    if qa_on in text:
        return
    text = text.replace(qa_off + "\n", "").replace(qa_off, "")
    if not text.endswith("\n"):
        text += "\n"
    text += qa_on + "\n"
    sdk.write_text(text, encoding="utf-8")


def idf_shell(args: list[str], cwd: Path, timeout: int = 900) -> subprocess.CompletedProcess:
    if args and args[0] == "build":
        ensure_qa_sdkconfig(cwd)
        args = ["reconfigure"] + args
    qa_cmd = cwd / "build_idf55_qa.cmd"
    idf_args = " ".join(args)
    # shell=True: export.bat fails under cmd /c with piped stdout (IDF 5.5 on Windows).
    inner = f'cd /d "{cwd}" && call "{qa_cmd}" {idf_args}'
    return subprocess.run(
        inner,
        shell=True,
        capture_output=True,
        text=True,
        timeout=timeout,
        encoding="utf-8",
        errors="replace",
    )


def load_golden() -> dict:
    p = GOLDEN / "scenes.json"
    if p.is_file():
        return json.loads(p.read_text(encoding="utf-8"))
    return {"scenes": ["boot", "home", "provision", "wifi", "pair"], "fingerprints": {}}


def save_golden(data: dict) -> None:
    GOLDEN.mkdir(parents=True, exist_ok=True)
    (GOLDEN / "scenes.json").write_text(json.dumps(data, indent=2), encoding="utf-8")


def fp_distance(a: str, b: str) -> int:
    if len(a) != len(b):
        return 999
    return sum(1 for x, y in zip(a, b) if x != y)


def fp_ui_metrics(hex_fp: str) -> dict:
    """Heuristic UI quality: contrast + non-blank screen (8x8 luminance grid)."""
    if len(hex_fp) != 128:
        return {"ok": False, "reason": "bad_fp_len"}
    vals = [int(hex_fp[i : i + 2], 16) for i in range(0, 128, 2)]
    lo, hi = min(vals), max(vals)
    spread = hi - lo
    mean = sum(vals) / len(vals)
    blank = spread < 8
    washed = spread < 18 and (mean > 240 or mean < 12)
    return {
        "ok": not blank and not washed,
        "spread": spread,
        "mean_lum": round(mean, 1),
        "blank": blank,
        "low_contrast": washed,
    }


def http_json(url: str, method: str = "GET", body: bytes | None = None, timeout: float = 8.0) -> dict:
    req = urllib.request.Request(url, data=body, method=method)
    if body is not None:
        req.add_header("Content-Type", "text/plain")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def monitor_serial(
    port: str,
    seconds: float,
    baud: int = 2000000,
    on_line: callable | None = None,
) -> str:
    if serial is None:
        raise RuntimeError("pyserial not installed; pip install -r qa/requirements.txt")
    buf: list[str] = []
    end = time.time() + seconds
    with serial.Serial(port, baud, timeout=0.5) as ser:
        while time.time() < end:
            line = ser.readline()
            if not line:
                continue
            try:
                text = line.decode("utf-8", errors="replace").rstrip()
            except Exception:
                continue
            buf.append(text)
            print(text)
            if on_line:
                on_line(text)
    return "\n".join(buf)


def http_with_retries(base: str, path: str, method: str = "GET", body: bytes | None = None, tries: int = 5) -> dict:
    last: Exception | None = None
    for i in range(tries):
        try:
            return http_json(f"{base}{path}", method=method, body=body, timeout=10.0)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
            last = e
            time.sleep(1.5 + i * 0.5)
    raise last or RuntimeError("http failed")


def try_wifi_connect(ssid: str) -> bool:
    """Best-effort Windows WLAN connect to SoftAP (may need admin)."""
    profile = f"""<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
    <name>{ssid}</name>
    <SSIDConfig><SSID><name>{ssid}</name></SSID></SSIDConfig>
    <connectionType>ESS</connectionType>
    <connectionMode>auto</connectionMode>
    <MSM><security><authEncryption><authentication>open</authentication>
    <encryption>none</encryption></authEncryption></security></MSM>
</WLANProfile>"""
    prof_path = ROOT / "_wlan_profile.xml"
    prof_path.write_text(profile, encoding="utf-8")
    for args in (
        ["netsh", "wlan", "add", "profile", f"filename={prof_path}"],
        ["netsh", "wlan", "connect", f"name={ssid}"],
    ):
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr or r.stdout)
    time.sleep(4)
    return True


def qa_http_scenes(base: str, scenes: list[str], golden: dict, result: Result, bootstrap: bool) -> None:
    fps: dict = golden.setdefault("fingerprints", {})
    for scene in scenes:
        try:
            http_with_retries(base, "/qa/ping", tries=5)
            http_with_retries(base, "/qa/scene", method="POST", body=scene.encode("utf-8"), tries=8)
            time.sleep(3.5)
            data = http_with_retries(base, "/qa/fp", tries=10)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
            result.fail(f"http_fp_{scene}", str(e))
            continue
        if not data.get("ok"):
            result.fail(f"http_fp_{scene}", "device returned ok=false")
            continue
        hex_fp = data.get("hex", "")
        metrics = fp_ui_metrics(hex_fp)
        if not metrics.get("ok"):
            result.fail(
                f"ui_aesthetic_{scene}",
                f"spread={metrics.get('spread')} mean={metrics.get('mean_lum')}",
            )
        else:
            result.pass_(f"ui_aesthetic_{scene}", f"spread={metrics.get('spread')}")
        ref = fps.get(scene)
        if bootstrap or not ref:
            fps[scene] = hex_fp
            golden.setdefault("ui_metrics", {})[scene] = metrics
            result.pass_(f"golden_bootstrap_{scene}", hex_fp[:32] + "...")
            continue
        dist = fp_distance(ref, hex_fp)
        if dist <= 12:
            result.pass_(f"fp_{scene}", f"hamming_cells={dist}")
        else:
            result.fail(f"fp_{scene}", f"hamming_cells={dist} expected~{ref[:32]} got~{hex_fp[:32]}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Pager watch QA runner")
    ap.add_argument("--port", default="COM7")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-flash", action="store_true")
    ap.add_argument("--bootstrap-golden", action="store_true", help="Record fingerprints as golden")
    ap.add_argument("--monitor-sec", type=float, default=55.0)
    ap.add_argument("--ap-ip", default="192.168.4.1")
    ap.add_argument("--device-ip", default="", help="Skip log parse; use this STA IP for /qa HTTP")
    args = ap.parse_args()

    result = Result()
    REPORTS.mkdir(parents=True, exist_ok=True)
    golden = load_golden()
    scenes = golden.get("scenes", ["boot", "provision", "wifi", "pair"])

    if not args.skip_build:
        r = idf_shell(["build"], PROJECT)
        if r.returncode != 0:
            print(r.stdout[-8000:])
            print(r.stderr[-8000:])
            result.fail("build", "idf build failed")
            write_report(result, args)
            return 1
        result.pass_("build", "CONFIG_PAGER_QA=y")

    if not args.skip_flash:
        r = idf_shell(["-p", args.port, "flash"], PROJECT, timeout=300)
        if r.returncode != 0:
            print(r.stdout[-4000:])
            print(r.stderr[-4000:])
            result.fail("flash", f"COM {args.port}")
            write_report(result, args)
            return 1
        result.pass_("flash", args.port)
        time.sleep(2)

    device_ip_live: list[str | None] = [None]
    device_ip_since: list[float] = [0.0]
    http_done = threading.Event()
    HTTP_SETTLE_SEC = 10.0

    def on_serial_line(text: str) -> None:
        if device_ip_live[0]:
            return
        m = QA_HTTP_RE.search(text)
        if m:
            device_ip_live[0] = m.group(1)
            device_ip_since[0] = time.time()
            return
        if args.device_ip and "HTTP /qa/* registered" in text:
            device_ip_live[0] = args.device_ip
            device_ip_since[0] = time.time()

    def http_worker(base_url: str) -> None:
        try:
            time.sleep(2.0)
            ping = http_with_retries(base_url, "/qa/ping")
            if ping.get("ok"):
                result.pass_("http_ping", base_url)
            qa_http_scenes(base_url, scenes, golden, result, args.bootstrap_golden)
            audio = http_with_retries(base_url, "/qa/audio")
            if audio.get("ok"):
                result.pass_("audio_selftest", audio.get("detail", ""))
            else:
                result.fail("audio_selftest", str(audio))
        except Exception as e:
            result.fail("http_qa", str(e))
        finally:
            http_done.set()

    log = ""
    try:
        http_thread: threading.Thread | None = None

        def maybe_start_http() -> None:
            nonlocal http_thread
            if http_thread or not device_ip_live[0]:
                return
            if device_ip_since[0] and (time.time() - device_ip_since[0]) < HTTP_SETTLE_SEC:
                return
            base_url = f"http://{device_ip_live[0]}"
            result.pass_("sta_qa_http", device_ip_live[0])
            http_thread = threading.Thread(target=http_worker, args=(base_url,), daemon=True)
            http_thread.start()

        if args.device_ip:
            device_ip_live[0] = args.device_ip.strip()
            device_ip_since[0] = time.time() - HTTP_SETTLE_SEC

        end = time.time() + args.monitor_sec
        if serial is None:
            raise RuntimeError("pyserial not installed; pip install -r qa/requirements.txt")
        with serial.Serial(args.port, 2000000, timeout=0.5) as ser:
            buf: list[str] = []
            while time.time() < end:
                line = ser.readline()
                if line:
                    try:
                        text = line.decode("utf-8", errors="replace").rstrip()
                    except Exception:
                        text = ""
                    if text:
                        buf.append(text)
                        print(text)
                        on_serial_line(text)
                        maybe_start_http()
                elif device_ip_live[0] and not http_thread:
                    maybe_start_http()
                if http_thread and http_done.is_set():
                    break
            log = "\n".join(buf)
        if http_thread:
            http_done.wait(timeout=120)
    except Exception as e:
        result.fail("serial_monitor", str(e))

    for m in BAD_LOG.finditer(log):
        result.fail("serial_health", m.group(0))
        result.log_hits.append(m.group(0))

    ui_warns = list(UI_WARN_LOG.finditer(log))
    if ui_warns:
        result.pass_("ui_runtime_health", f"warn_count={len(ui_warns)} (non-fatal while QA hardening)")
        for m in ui_warns[:3]:
            result.log_hits.append(m.group(0))
    else:
        result.pass_("ui_runtime_health", "clean")

    if "BS:Pager" in log or "QA: auto-start Pager" in log:
        result.pass_("pager_autostart", "seen in log")
    elif args.skip_flash:
        result.pass_("pager_autostart", "skip-flash (no boot log)")
    else:
        result.fail("pager_autostart", "Pager QA autostart log missing")

    ssid_m = SOFTAP_RE.search(log)
    if ssid_m and not http_done.is_set():
        ssid = ssid_m.group(1)
        result.pass_("softap", ssid)
        try_wifi_connect(ssid)
        threading.Thread(
            target=http_worker,
            args=(f"http://{args.ap_ip}",),
            daemon=True,
        ).start()
        http_done.wait(timeout=120)
    elif not http_done.is_set() and not device_ip_live[0]:
        result.fail("network", "no SoftAP or STA IP in log; HTTP tests skipped")

    if args.bootstrap_golden:
        save_golden(golden)
        result.pass_("save_golden", str(GOLDEN / "scenes.json"))

    write_report(result, args)
    print("\n=== QA", "PASS" if result.ok else "FAIL", "===")
    return 0 if result.ok else 1


def write_report(result: Result, args: argparse.Namespace) -> None:
    ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = REPORTS / f"report_{ts}.json"
    payload = {
        "ok": result.ok,
        "port": args.port,
        "steps": result.steps,
        "log_hits": result.log_hits,
    }
    out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Report: {out}")


if __name__ == "__main__":
    sys.exit(main())
