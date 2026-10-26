"""Decode a saved qa.shot capture into a PNG (bytes-swapped RGB565 aware)."""
import re
import struct
import sys
import zlib

path = sys.argv[1]
out = sys.argv[2]

data = open(path, 'rb').read()
text = data.decode('utf-8', 'replace')
m = re.search(r'qa\.shot (\d+) (\d+) (\d+) rgb565\r?\n', text)
w, h = int(m.group(1)), int(m.group(2))
end = text.index('qa.shot end')
body = text[m.end():end]
lines = [l for l in body.split('\n') if len(l.strip()) == w * 4]
print('rows', len(lines), 'of', h)

raw = bytearray()
for line in lines[:h]:
    b = bytes.fromhex(line.strip())
    for x in range(w):
        # The snapshot buffer holds byteswapped RGB565 as sent to the panel.
        p = (b[x * 2] << 8) | b[x * 2 + 1]
        r = ((p >> 11) & 31) * 255 // 31
        g = ((p >> 5) & 63) * 255 // 63
        bl = (p & 31) * 255 // 31
        raw += bytes((r, g, bl))

def chunk(tag, payload):
    return (struct.pack('>I', len(payload)) + tag + payload +
            struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
scan = b''.join(b'\x00' + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
       chunk(b'IDAT', zlib.compress(scan, 6)) + chunk(b'IEND', b''))
open(out, 'wb').write(png)
print('saved', out)