# testsrv.py — HTTP server with Range support + per-connection throttle for engine testing
import http.server, socketserver, sys, time, os

FILE = sys.argv[2] if len(sys.argv) > 2 else None
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
THROTTLE_BPS = int(sys.argv[3]) if len(sys.argv) > 3 else 0   # per-connection bytes/sec
BODY = None
if FILE and os.path.exists(FILE):
    BODY = open(FILE, 'rb').read()
else:
    BODY = bytes(64 * 1024 * 1024)  # 64MB of zeros

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_GET(self):
        rng = self.headers.get('Range')
        start, end = 0, len(BODY) - 1
        status = 200
        if rng:
            try:
                unit, _, spec = rng.partition('=')
                a, _, b = spec.partition('-')
                start = int(a) if a else 0
                end = int(b) if b else len(BODY) - 1
                status = 206
            except ValueError:
                pass
        data = BODY[start:end+1]
        self.send_response(status)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(data)))
        if status == 206:
            self.send_header('Content-Range', f'bytes {start}-{end}/{len(BODY)}')
        self.send_header('Accept-Ranges', 'bytes')
        self.end_headers()
        if THROTTLE_BPS > 0:
            chunk = THROTTLE_BPS // 10 or 1
            mv = memoryview(data)
            for i in range(0, len(data), chunk):
                self.wfile.write(mv[i:i+chunk])
                self.wfile.flush()
                time.sleep(0.1)
        else:
            self.wfile.write(data)

socketserver.ThreadingTCPServer.allow_reuse_address = True
with socketserver.ThreadingTCPServer(('127.0.0.1', PORT), H) as srv:
    print(f'serving {len(BODY)} bytes on :{PORT} throttle={THROTTLE_BPS}')
    srv.serve_forever()
