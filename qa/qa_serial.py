"""Drive the round watch over its QA serial console.

Usage:
  python qa_serial.py COM8 shot out.png          # frame dump -> png
  python qa_serial.py COM8 swipe right|left      # horizontal page flip
  python qa_serial.py COM8 ptt 2500              # simulated side-key hold
  python qa_serial.py COM8 wifi <ssid> <pass>    # provision
  python qa_serial.py COM8 raw <text>            # send a raw line
"""
import sys
import time

import serial


def open_port(port, baud=2000000):
    s = serial.Serial(port, baud, timeout=0.05)
    s.setDTR(False)
    s.setRTS(False)
    # Opening the CDC port resets the board; wait for the app to finish
    # booting (AP up) before sending anything, or the line is swallowed.
    end = time.time() + 15
    seen = bytearray()
    while time.time() < end:
        seen += s.read(4096)
        text = seen.decode('utf-8', 'replace')
        if 'softap Pager' in text or 'qa http' in text:
            break
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def read_until(s, markers, timeout):
    end = time.time() + timeout
    buf = bytearray()
    while time.time() < end:
        chunk = s.read(8192)
        if chunk:
            buf += chunk
            text = buf.decode('utf-8', 'replace')
            for m in markers:
                if text.rstrip().endswith(m):
                    return text
    return buf.decode('utf-8', 'replace')


def cmd_shot(s, out_path):
    s.reset_input_buffer()
    s.write(b'qa.shot\n')
    text = read_until(s, ['qa.shot end', 'qa.shot fail'], 30)
    lines = text.splitlines()
    header = next((l for l in lines if l.startswith('qa.shot ')), None)
    if not header or 'end' not in text:
        print(text[-2000:])
        raise SystemExit('shot failed')
    _, w, h, stride, _fmt = header.split()
    w, h, stride = int(w), int(h), int(stride)
    rows = [l for l in lines if len(l) == w * 4]
    if len(rows) != h:
        print('rows %d != %d' % (len(rows), h))
    import struct
    import zlib
    raw = bytearray()
    for line in rows[:h]:
        data = bytes.fromhex(line)
        for x in range(w):
            p = data[x * 2] | (data[x * 2 + 1] << 8)
            r = ((p >> 11) & 31) * 255 // 31
            g = ((p >> 5) & 63) * 255 // 63
            b = (p & 31) * 255 // 31
            raw += bytes((r, g, b))
    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    scan = b''.join(b'\x00' + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
           chunk(b'IDAT', zlib.compress(scan, 6)) + chunk(b'IEND', b''))
    with open(out_path, 'wb') as f:
        f.write(png)
    print('saved %s (%dx%d)' % (out_path, w, h))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        raise SystemExit(2)
    port, cmd = sys.argv[1], sys.argv[2]
    s = open_port(port)
    try:
        if cmd == 'shot':
            cmd_shot(s, sys.argv[3] if len(sys.argv) > 3 else 'shot.png')
            return
        if cmd == 'swipe':
            s.write(('qa.swipe notused\n').encode()) if False else None
            body = sys.argv[3]
            # The HTTP handler is not reachable over serial; the swipe flag
            # is set through the dedicated line below.
            s.write(('qa.%s %s\n' % (cmd, body)).encode())
        elif cmd == 'raw':
            s.write((sys.argv[3] + '\n').encode())
        else:
            s.write(('qa.%s %s\n' % (cmd, ' '.join(sys.argv[3:]))).encode())
        time.sleep(0.5)
        out = read_until(s, [], 3)
        tail = out.strip().splitlines()[-12:]
        print('\n'.join(tail))
    finally:
        s.close()


if __name__ == '__main__':
    main()