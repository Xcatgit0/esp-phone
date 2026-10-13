#!/usr/bin/env python3
"""Tiny HTTP/1.1 server for tests/http.lua (python3 http_server.py [port])."""
import os, struct, sys, time, json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.environ.get("AUDIO_ROOT", ".")

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def _send(self, code, body=b"", headers=None, head=False):
        self.send_response(code)
        for k, v in (headers or []):
            self.send_header(k, v)
        if "Content-Length" not in dict(headers or []) and code not in (204, 304):
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head and code not in (204, 304):
            self.wfile.write(body)

    def _chunked(self, parts):
        self.send_response(200)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for p in parts:
            self.wfile.write(b"%x\r\n" % len(p) + p + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")

    def _handle(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        path, _, query = self.path.partition("?")
        head = self.command == "HEAD"
        if path == "/hello":
            return self._send(200, b"hello world", [("Content-Type", "text/plain"), ("X-Test", "abc"),
                                                     ("X-Multi", "a"), ("X-Multi", "b")], head)
        if path == "/echo":
            txt = "method=%s\nbody=%s\nx-custom=%s\ncontent-type=%s\n" % (
                self.command, body.decode(errors="replace"), self.headers.get("X-Custom", ""), self.headers.get("Content-Type", ""))
            return self._send(200, txt.encode(), [("Content-Type", "text/plain")])
        if path == "/headers":
            return self._send(200, "\n".join("%s: %s" % kv for kv in self.headers.items()).encode())
        if path.startswith("/status/"):
            code = int(path.split("/")[2])
            return self._send(code, ("status %d" % code).encode())
        if path == "/redir":   return self._send(302, b"", [("Location", "/hello")])
        if path == "/redir2":  return self._send(301, b"", [("Location", "/redir")])
        if path == "/loop":    return self._send(302, b"", [("Location", "/loop")])
        if path == "/redir-post": return self._send(302, b"", [("Location", "/echo")])
        if path == "/redir-307":  return self._send(307, b"", [("Location", "/echo")])
        if path == "/redir-rel":  return self._send(302, b"", [("Location", "hello")])
        if path == "/slow":
            time.sleep(2)
            return self._send(200, b"slow")
        if path == "/big":
            size = int(query.split("=")[1]) if query else 100000
            return self._send(200, b"x" * size)
        if path == "/chunked":
            return self._chunked([b"chunk1", b"chunk2", b"chunk3"])
        if path == "/chunked-big":
            return self._chunked([b"y" * 10000] * 10)
        if path == "/empty": return self._send(204)
        if path.startswith("/file/"):                       # static files from $AUDIO_ROOT with Content-Length
            fp = os.path.join(ROOT, os.path.basename(path))
            if not os.path.exists(fp): return self._send(404, b"no such file")
            return self._send(200, open(fp, "rb").read(), [("Content-Type", "audio/mpeg")], head)
        if path.startswith("/redir-file/"):
            return self._send(302, b"", [("Location", "/file/" + os.path.basename(path))])
        if path == "/token-file":                           # needs a request header
            if self.headers.get("X-Token") != "abc": return self._send(403, b"forbidden")
            return self._send(200, open(os.path.join(ROOT, "mono22k.wav"), "rb").read())
        if path == "/radio":                                # endless 500 Hz WAV stream, produced in real time
            self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.send_header("Connection", "close"); self.end_headers()
            self.close_connection = True
            rate = 22050
            hdr = b"RIFF" + struct.pack("<I", 0xFFFFFFFF) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", 0xFFFFFFFF)
            import math
            try:
                self.wfile.write(hdr)
                n = 0
                for _ in range(300):                        # 30 s at most
                    chunk = b"".join(struct.pack("<h", int(0.4 * 32767 * math.sin(2 * math.pi * 500 * (n + i) / rate))) for i in range(rate // 10))
                    n += rate // 10
                    self.wfile.write(chunk); self.wfile.flush(); time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if path == "/stall-start":                          # accepts the connection, answers only after 6 s
            time.sleep(6)
            return self._send(200, open(os.path.join(ROOT, "mono22k.wav"), "rb").read())
        if path == "/stall-mid":                            # first half at once, pause 1.5 s, second half
            data = open(os.path.join(ROOT, "mono22k.wav"), "rb").read()
            self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers()
            try:
                self.wfile.write(data[:len(data) // 2]); self.wfile.flush(); time.sleep(1.5)
                self.wfile.write(data[len(data) // 2:])
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        return self._send(404, b"not found")

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    do_GET = do_POST = do_PUT = do_DELETE = do_PATCH = do_HEAD = _handle

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
