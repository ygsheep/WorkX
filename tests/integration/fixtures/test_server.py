"""
@file test_server.py
@brief HTTP 测试服务器，用于 HttpClient 测试
@details 提供 GET/流式 POST/超时/错误 端点，启动后打印端口号
"""

import json
import time
import socket
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

HOST = "127.0.0.1"


class TestHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # suppress logs

    def _send_json(self, code, data):
        body = json.dumps(data).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_sse_chunk(self, data: str):
        self.wfile.write(f"data: {json.dumps(data)}\n\n".encode("utf-8"))
        self.wfile.flush()

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/v1/models":
            self._send_json(200, {
                "object": "list",
                "data": [
                    {"id": "gpt-4o", "owned_by": "openai", "context_length": 128000},
                    {"id": "gpt-3.5-turbo", "owned_by": "openai"},
                ]
            })
        elif parsed.path == "/echo":
            data = {"method": "GET", "path": self.path}
            self._send_json(200, data)
        elif parsed.path == "/delay":
            import time
            time.sleep(3)
            self._send_json(200, {"status": "delayed"})
        elif parsed.path == "/status/404":
            self._send_json(404, {"error": "not found"})
        elif parsed.path == "/status/500":
            self._send_json(500, {"error": "server error"})
        else:
            self._send_json(200, {"message": "hello"})

    def do_POST(self):
        parsed = urlparse(self.path)
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else b""

        if parsed.path == "/v1/chat/completions" and \
           self.headers.get("Accept", "") == "text/event-stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            # Connection: close 让客户端在响应结束后立即关闭连接，
            # 避免 keep-alive 模式下客户端等待更多数据导致超时
            self.send_header("Connection", "close")
            self.end_headers()

            chunks = [
                {"choices": [{"index": 0, "delta": {"content": "Hello"}, "finish_reason": None}]},
                {"choices": [{"index": 0, "delta": {"content": " world"}, "finish_reason": None}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
            ]
            for c in chunks:
                self._send_sse_chunk(c)
                import time
                time.sleep(0.05)
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
            return

        elif parsed.path == "/echo":
            try:
                req = json.loads(body) if body else {}
            except json.JSONDecodeError:
                req = {"raw": body.decode("utf-8", errors="replace")}
            self._send_json(200, {"method": "POST", "path": self.path, "body": req})

        elif parsed.path == "/stream-timeout":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(b"data: {\"msg\":\"start\"}\n\n")
            self.wfile.flush()
            import time
            time.sleep(10)
            self.wfile.write(b"data: {\"msg\":\"too late\"}\n\n")
            self.wfile.flush()
            return

        else:
            self._send_json(200, {"status": "ok"})


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def run_server(port: int, ready_event: threading.Event):
    server = HTTPServer((HOST, port), TestHandler)
    ready_event.set()
    server.serve_forever()


if __name__ == "__main__":
    port = find_free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_server, args=(port, ready), daemon=True)
    t.start()
    ready.wait()
    print(f"TEST_SERVER_PORT={port}", flush=True)
    try:
        while True:
            import time
            time.sleep(1)
    except KeyboardInterrupt:
        pass