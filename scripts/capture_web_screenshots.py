"""Capture Pager Web UI screenshots for docs and GitHub social preview."""
from __future__ import annotations

from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
GALLERY = ROOT / "docs" / "gallery"
URL = "http://127.0.0.1:8765/"


def main() -> None:
    GALLERY.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 1280, "height": 800})
        page.goto(URL, wait_until="networkidle", timeout=60_000)
        page.wait_for_timeout(1500)
        page.screenshot(path=str(GALLERY / "web-agent-center-live.png"), full_page=False)
        page.set_viewport_size({"width": 1440, "height": 900})
        page.screenshot(path=str(GALLERY / "web-agent-center-full.png"), full_page=True)
        browser.close()
    print("saved", GALLERY / "web-agent-center-live.png")


if __name__ == "__main__":
    main()
