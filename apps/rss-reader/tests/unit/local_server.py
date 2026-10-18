#!/usr/bin/env python3
"""Tiny local HTTP server for the CurlClient tests (test_fetch.cpp, run by run_tests.sh).

Usage: local_server.py <port-file>; it binds an ephemeral port on 127.0.0.1, writes the port number to <port-file>
and serves until it is killed. Routes:
  /feed         200 RSS, ETag "v1", Last-Modified; 304 when If-None-Match / If-Modified-Since match
  /gzip         200 RSS gzip-encoded (Content-Encoding: gzip)
  /redirect     302 -> /feed        /redirect3 -> /redirect2 -> /redirect1 -> /feed (3 hops)
  /loop         302 -> /loop        /to-ftp       302 -> ftp://127.0.0.1/x
  /big          6 MB of text, Content-Length set      /bigchunk  endless chunked body
  /slow         waits 5 s, then 200
  /404, /500    error statuses
  /png          a 1x1 PNG (image/png)   /fakeimg  HTML labelled text/html   /bigimg 3 MB image/png
  /ua           echoes the User-Agent and the Accept header as the body
"""
import gzip
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RSS = b"""<?xml version="1.0"?><rss version="2.0"><channel><title>Local</title><link>http://127.0.0.1/</link>
<item><title>One</title><guid>1</guid><pubDate>Sun, 18 Oct 2026 15:10:05 GMT</pubDate></item></channel></rss>"""
PNG = bytes.fromhex("89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6360000002000001e221bc330000000049454e44ae426082")


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, status, body=b"", ctype="text/plain", extra=()):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def redirect(self, to):
        self.send(302, b"", extra=[("Location", to)])

    def do_GET(self):
        p = self.path
        if p == "/feed":
            if self.headers.get("If-None-Match") == '"v1"' or self.headers.get("If-Modified-Since") == "Sun, 18 Oct 2026 15:00:00 GMT":
                return self.send(304, b"", extra=[("ETag", '"v1"')])
            return self.send(200, RSS, "application/rss+xml; charset=utf-8", [("ETag", '"v1"'), ("Last-Modified", "Sun, 18 Oct 2026 15:00:00 GMT")])
        if p == "/gzip":
            return self.send(200, gzip.compress(RSS), "application/xml", [("Content-Encoding", "gzip")])
        if p == "/redirect":
            return self.redirect("/feed")
        if p == "/redirect3":
            return self.redirect("/redirect2")
        if p == "/redirect2":
            return self.redirect("/redirect1")
        if p == "/redirect1":
            return self.redirect("/feed")
        if p == "/loop":
            return self.redirect("/loop")
        if p == "/to-ftp":
            return self.redirect("ftp://127.0.0.1/x")
        if p == "/big":
            return self.send(200, b"x" * (6 * 1024 * 1024), "text/plain")
        if p == "/bigchunk":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            try:
                for _ in range(10000):
                    chunk = b"y" * 65536
                    self.wfile.write(b"%x\r\n" % len(chunk) + chunk + b"\r\n")
            except Exception:
                pass
            return
        if p == "/slow":
            time.sleep(5)
            return self.send(200, b"late")
        if p == "/404":
            return self.send(404, b"nope")
        if p == "/500":
            return self.send(500, b"boom")
        if p == "/png":
            return self.send(200, PNG, "image/png")
        if p == "/fakeimg":
            return self.send(200, b"<html>not an image</html>", "text/html")
        if p == "/bigimg":
            return self.send(200, PNG + b"\0" * (3 * 1024 * 1024), "image/png")
        if p == "/ua":
            return self.send(200, ("UA=%s\nAccept=%s\n" % (self.headers.get("User-Agent"), self.headers.get("Accept"))).encode())
        self.send(404, b"unknown route")


class Quiet(ThreadingHTTPServer):
    def handle_error(self, request, client_address):   # clients that hang up early (size caps, timeouts) are normal here
        pass


if __name__ == "__main__":
    srv = Quiet(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    with open(sys.argv[1], "w") as f:
        f.write(str(srv.server_address[1]))
    srv.serve_forever()
