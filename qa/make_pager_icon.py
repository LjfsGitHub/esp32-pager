import os
from PIL import Image, ImageDraw, ImageFont

S = 112
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((1, 1, S - 2, S - 2), radius=32, fill=(23, 22, 19, 255))
font = None
for p in (
    r"C:\Windows\Fonts\segoeuib.ttf",
    r"C:\Windows\Fonts\arialbd.ttf",
    r"C:\Windows\Fonts\bahnschrift.ttf",
):
    if os.path.exists(p):
        font = ImageFont.truetype(p, 68)
        print("font", p)
        break
if font is None:
    raise SystemExit("no font")
text = "P"
bbox = d.textbbox((0, 0), text, font=font)
tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
x = (S - tw) / 2 - bbox[0]
y = (S - th) / 2 - bbox[1] - 2
d.text((x, y), text, font=font, fill=(248, 243, 233, 255))
dot = 16
dx = S - 22 - dot
dy = 16
d.ellipse((dx, dy, dx + dot, dy + dot), fill=(255, 92, 53, 255))
png = r"D:\ESP32\esp32-pc\qa\shots\pager_icon.png"
im.save(png)
pixels = list(im.getdata())
lines = [
    "/* Pager launcher mark: ink tile, cream P, accent dot. */",
    "#include \"lvgl.h\"",
    "static const uint8_t pager_icon_112_map[] = {",
]
chunk = []
for r, g, b, a in pixels:
    chunk.extend((b, g, r, a))
    if len(chunk) >= 48:
        lines.append("    " + ", ".join(f"0x{v:02x}" for v in chunk) + ",")
        chunk = []
if chunk:
    lines.append("    " + ", ".join(f"0x{v:02x}" for v in chunk) + ",")
lines.extend(
    [
        "};",
        "const lv_image_dsc_t pager_icon_112 = {",
        "    .header.cf = LV_COLOR_FORMAT_ARGB8888,",
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        "    .header.w = 112,",
        "    .header.h = 112,",
        "    .data_size = 112 * 112 * 4,",
        "    .data = pager_icon_112_map,",
        "};",
        "",
    ]
)
path = r"D:\pager_firmware_2_16\examples\esp-idf\03_esp-brookesia\components\brookesia_app_pager\pager\pager_icon.c"
with open(path, "w", newline="\n") as f:
    f.write("\n".join(lines))
print("wrote", path, os.path.getsize(path))
