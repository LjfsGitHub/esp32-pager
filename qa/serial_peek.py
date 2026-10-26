import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM8'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 2000000

s = serial.Serial(port, baud, timeout=1)
time.sleep(0.3)
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)
end = time.time() + 5
data = b''
while time.time() < end:
    chunk = s.read(4096)
    if chunk:
        data += chunk
s.close()
text = data.decode('utf-8', 'replace')
print(text[-4000:])
