"""Walk every pager page on the round watch over the QA serial console.

For each scene: switch -> settle -> qa.shot -> decode -> save png.
"""
import re
import struct
import sys
import time
import zlib

import serial

PORT = 'COM8'
OUT = r'qa\shots'

PAGES = [
    ('home', None, 'round_home'),
    ('home', 'right', 'round_stats'),
    ('home', 'left', 'round_approval'),
    ('provision', None, 'round_provision'),
    ('pair', None, 'round_pair'),
    ('wifi', None, 'round_wifi'),
]


def open_port():
    s = serial.Serial(PORT, 2000000, timeout=0.1)
    s.setDTR(False)
    s.setRTS(False)
    end = time.time() + 14
    seen = bytearray()
    while time.time() < end:
        seen += s.read(4096)
        if b'softap Pager' in seen or b'qa http' in seen:
            break
    time.sleep(0.4)
    s.reset_input_buffer()
    return s


def command(s, line, wait=1.0):
    s.reset_input_buffer()
    s.write((line + '\n').encode())
    time.sleep(wait)
    tail = s.read(8192).decode('utf-8', 'replace')
    return tail


def shot(s, name):
    s.reset_input_buffer()
    s.write(b'qa.shot\n')
    end = time.time() + 40
    buf = bytearray()
    while time.time() < end:
        c = s.read(8192)
        if c:
            buf += c
        if b'qa.shot end' in bytes(buf):
            break
    time.sleep(0.3)
    # Keep draining until the console is quiet, the end marker may have
    # been followed by a few log lines the shot itself did not include.
    end2 = time.time() + 2
    while time.time() < end2:
        c = s.read(8192)
        if c:
            buf += c
    text = buf.decode('utf-8', 'replace')
    m = re.search(r'qa\.shot (\d+) (\d+) (\d+) rgb565\r?\n', text)
    if not m or 'qa.shot end' not in text:
        print('  shot FAIL', name)
        return False
    w, h, stride = int(m.group(1)), int(m.group(2)), int(m.group(3))
    body = text[m.end():text.index('qa.shot end')]
    # USB packet boundaries split rows across two physical lines; strip all
    # non-hex, non-ascii characters then re-cut into w*4 rows.
    blob = re.sub(r'[^0-9a-f]', '', body)
    rows = [blob[i:i + w * 4] for i in range(0, len(blob) - w * 4 + 1, w * 4)]
    rows = rows[:h]
    raw = bytearray()
    for line in rows:
        data = bytes.fromhex(line)
        for x in range(w):
            p = (data[x * 2] << 8) | data[x * 2 + 1]
            r = ((p >> 11) & 31) * 255 // 31
            g = ((p >> 5) & 63) * 255 // 63
            b = (p & 31) * 255 // 31
            raw += bytes((r, g, b))
    if len(raw) < w * h * 3:
        print('  short frame', name, len(raw))
        return False

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    scan = b''.join(b'\x00' + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
           chunk(b'IDAT', zlib.compress(scan, 6)) + chunk(b'IEND', b''))
    path = OUT + '\\' + name + '.png'
    open(path, 'wb').write(png)
    print('  saved', path, 'rows', len(rows))
    return True


def main():
    s = open_port()
    ok = 0
    try:
        for scene, swipe, name in PAGES:
            r = command(s, 'qa.scene %s' % scene, 0.8)
            good = 'qa.scene ok' in r
            print(scene, 'switch', 'ok' if good else 'FAIL: ' + r[-80:].strip())
            if not good:
                continue
            time.sleep(0.4)
            if swipe:
                r = command(s, 'qa.swipe %s' % swipe, 0.8)
                time.sleep(0.4)
            if shot(s, name):
                ok += 1
    finally:
        s.close()
    print('done %d/%d' % (ok, len(PAGES)))


if __name__ == '__main__':
    main()