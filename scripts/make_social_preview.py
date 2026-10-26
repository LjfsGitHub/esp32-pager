"""Build 1280x640 GitHub social preview from gallery assets."""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
GALLERY = ROOT / "docs" / "gallery"
OUT = GALLERY / "github-social-preview.png"
W, H = 1280, 640


def load(path: Path, size: tuple[int, int]) -> Image.Image:
    img = Image.open(path).convert("RGBA")
    img.thumbnail(size, Image.Resampling.LANCZOS)
    return img


def main() -> None:
    bg = Image.new("RGB", (W, H), (250, 246, 238))
    draw = ImageDraw.Draw(bg)
    web = GALLERY / "web-agent-center-live.png"
    if not web.is_file():
        web = GALLERY / "web-agent-center.png"
    device = GALLERY / "device-square-home.png"
    board = ROOT / "docs" / "assets" / "ESP32-S3-Touch-AMOLED-2.16-details-1.jpg"

    left = load(web, (720, 520))
    bg.paste(left, (24, 60), left if left.mode == "RGBA" else None)

    if device.is_file():
        dev = load(device, (220, 220))
        bg.paste(dev, (980, 120), dev)

    title = "ESP32 Pager — Open Source"
    sub = "AMOLED watch · Voice PTT · AI Agent Bridge (MIT)"
    try:
        font_t = ImageFont.truetype("arial.ttf", 36)
        font_s = ImageFont.truetype("arial.ttf", 22)
    except OSError:
        font_t = ImageFont.load_default()
        font_s = ImageFont.load_default()
    draw.text((760, 380), title, fill=(30, 30, 30), font=font_t)
    draw.text((760, 430), sub, fill=(80, 80, 80), font=font_s)
    draw.text((760, 480), "Hermes · Cursor · Codex · Claude · WorkBuddy", fill=(120, 90, 60), font=font_s)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    bg.save(OUT, "PNG", optimize=True)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
