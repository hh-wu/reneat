# wsclient.py — minimal WebSocket client to test reneat's browser bridge
import socket, base64, os, json, sys, struct

HOST, PORT = '127.0.0.1', 20099
url = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8899/bigfile.bin'

s = socket.create_connection((HOST, PORT), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
req = (f"GET / HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
       f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
       f"Sec-WebSocket-Version: 13\r\n\r\n")
s.sendall(req.encode())
resp = b''
while b'\r\n\r\n' not in resp:
    resp += s.recv(4096)
head = resp.decode(errors='replace').split('\r\n\r\n')[0]
print('handshake:', head.split('\r\n')[0])
assert '101' in head, head

payload = json.dumps({"url": url, "referer": "https://example.com/page",
                      "filename": "ws-test-file.bin"}).encode()
mask = os.urandom(4)
masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
n = len(payload)
hdr = bytes([0x81, 0x80 | (126 if n >= 126 else n)])
if n >= 126:
    hdr = bytes([0x81, 0x80 | 126]) + struct.pack('>H', n)
s.sendall(hdr + mask + masked)

# read reply frame
h = s.recv(2)
op, ln = h[0] & 0x0f, h[1] & 0x7f
if ln == 126:
    ln = struct.unpack('>H', s.recv(2))[0]
data = b''
while len(data) < ln:
    data += s.recv(ln - len(data))
print('reply opcode', op, ':', data.decode())
s.close()
