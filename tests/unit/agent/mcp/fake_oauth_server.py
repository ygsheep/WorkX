#!/usr/bin/env python3
"""Fake OAuth 2.0 token server for unit tests (Issue #27 M4).

Serves POST /token (application/x-www-form-urlencoded):
  - grant_type=client_credentials  -> access_token "cc-token"
  - grant_type=authorization_code  -> access_token "ac-token" (requires code_verifier)
  - grant_type=refresh_token       -> access_token "rt-token"

Prints "PORT=<n>" on startup so the test can discover the dynamic port.
"""

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs

PORT = int(os.environ.get("FAKE_OAUTH_PORT", "0"))
# 1=返回 expires_in=1（测试 token 过期后自动刷新）
SHORT_EXPIRY = os.environ.get("FAKE_OAUTH_SHORT_EXPIRY", "0") == "1"


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        form = parse_qs(body)

        grant = form.get("grant_type", [""])[0]
        if grant == "client_credentials":
            token = "cc-token"
        elif grant == "authorization_code":
            verifier = form.get("code_verifier", [""])[0]
            if not verifier:
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b'{"error":"invalid_request"}')
                return
            token = "ac-token"
        elif grant == "refresh_token":
            token = "rt-token"
        else:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b'{"error":"unsupported_grant_type"}')
            return

        payload = json.dumps({
            "access_token": token,
            "token_type": "Bearer",
            "expires_in": 1 if SHORT_EXPIRY else 3600,
            "refresh_token": "refresh-123",
        })
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(payload.encode())

    def log_message(self, fmt, *args):
        pass  # 静默，避免测试输出刷屏


def main():
    server = HTTPServer(("127.0.0.1", PORT), Handler)
    print("PORT=" + str(server.server_address[1]), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
