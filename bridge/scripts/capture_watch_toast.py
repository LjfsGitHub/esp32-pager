"""Closed-loop: open PC watch sim, push /api/notify, screenshot device screen."""
from __future__ import annotations

import sys
import time
from pathlib import Path

OUT = Path(r"D:\ESP32\esp32-pc\assets\watch_toast_closed_loop.png")
OUT.parent.mkdir(parents=True, exist_ok=True)

TITLE = "闭环测试"
BODY = "通知 API → WS → 手表气泡"


def main() -> int:
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("playwright not installed", file=sys.stderr)
        return 1

    with sync_playwright() as p:
        try:
            browser = p.chromium.launch(headless=True)
        except Exception as exc:
            print(f"chromium launch failed: {exc}", file=sys.stderr)
            print("run: python -m playwright install chromium", file=sys.stderr)
            return 2
        page = browser.new_page(viewport={"width": 1280, "height": 900})
        page.goto("http://127.0.0.1:8765/", wait_until="networkidle", timeout=60000)
        page.wait_for_timeout(2500)
        page.evaluate(
            """async ([title, body]) => {
            const r = await fetch('/api/notify', {
              method: 'POST',
              headers: {'Content-Type': 'application/json'},
              body: JSON.stringify({title, body, level: 'ok', ttlMs: 15000, speak: false})
            });
            if (!r.ok) throw new Error(await r.text());
          }""",
            [TITLE, BODY],
        )
        page.wait_for_function(
            """() => {
            const el = document.getElementById('screenNotify');
            return el && !el.hidden && (document.getElementById('screenNotifyTitle')?.textContent || '').includes('闭环');
          }""",
            timeout=15000,
        )
        time.sleep(0.4)
        hardware = page.locator("#hardware")
        hardware.screenshot(path=str(OUT))
        browser.close()
    print(str(OUT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
