# wsclient_official.py — simulate the OFFICIAL NeatDM extension (neatextension.v1)
import socket, base64, os, struct, sys

HOST, PORT, PATH = '127.0.0.1', 10007, '/download'
url = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8899/bigfile.bin'

s = socket.create_connection((HOST, PORT), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
req = (f"GET {PATH} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
       f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
       f"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: neatextension.v1\r\n\r\n")
s.sendall(req.encode())
resp = b''
while b'\r\n\r\n' not in resp:
    resp += s.recv(4096)
head = resp.decode(errors='replace').split('\r\n\r\n')[0]
print('handshake:', head.split('\r\n')[0])
for line in head.split('\r\n'):
    if 'protocol' in line.lower():
        print('  subprotocol echo:', line.strip())
assert '101' in head
assert 'neatextension.v1' in head, 'subprotocol not echoed!'

def send_frame(payload: bytes):
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    n = len(payload)
    if n < 126:
        hdr = bytes([0x81, 0x80 | n])
    else:
        hdr = bytes([0x81, 0x80 | 126]) + struct.pack('>H', n)
    s.sendall(hdr + mask + masked)

def read_frame():
    h = s.recv(2)
    if not h or len(h) < 2:
        return None, None
    op, ln = h[0] & 0x0f, h[1] & 0x7f
    if ln == 126:
        ln = struct.unpack('>H', s.recv(2))[0]
    data = b''
    while len(data) < ln:
        chunk = s.recv(ln - len(data))
        if not chunk:
            break
        data += chunk
    return op, data

# read the server greeting (Version / nowaiting / ShowPanelChrome)
for _ in range(3):
    op, data = read_frame()
    print('greeting:', data.decode(errors='replace'))

# official numbered-field job message
job = (f"1:GET\r\n2:{url}\r\n6:normal\r\n4:Official Test\r\n"
       f"Origin: http://127.0.0.1:8899\r\n"
       f"Referer: http://127.0.0.1:8899/page\r\n"
       f"Cookie: session=abc123\r\n"
       f"9:MP4 File  64.00 MB\r\n")
send_frame(job.encode())
print('job sent (official format)')
s.close()
print('done')
