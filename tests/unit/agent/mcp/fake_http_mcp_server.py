#!/usr/bin/env python3
"""Fake MCP Streamable HTTP server for unit tests (Issue #27 M3).

Speaks JSON-RPC 2.0 over HTTP POST (Streamable HTTP transport).
Modes (controlled by env FAKE_MCP_HTTP_MODE):
  - "discover": respond to server/discover (2.0 stateless) [default]
  - "legacy":   reject server/discover with MethodNotFound, use initialize handshake
  - "sse":      respond with text/event-stream content type (2.0 discover)

Prints "PORT=<n>" on startup so the test can discover the dynamic port.
"""

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

MODE = os.environ.get("FAKE_MCP_HTTP_MODE", "discover")
PORT = int(os.environ.get("FAKE_MCP_HTTP_PORT", "0"))

# cache 模式：tools/list 首次返回 _meta.ttlMs，后续返回错误（验证客户端缓存）
_tools_list_count = 0


def make_result(req, result):
    return {"jsonrpc": "2.0", "id": req.get("id"), "result": result}


def make_error(req, code, message):
    return {"jsonrpc": "2.0", "id": req.get("id"),
            "error": {"code": code, "message": message}}


def handle(req):
    method = req.get("method", "")
    params = req.get("params", {}) or {}

    if method == "server/discover":
        if MODE == "legacy":
            return make_error(req, -32601, "Method not found")
        return make_result(req, {"protocolVersion": "2026-07-28"})

    if method == "initialize":
        return make_result(req, {
            "protocolVersion": params.get("protocolVersion", "2025-11-25"),
            "capabilities": {},
            "serverInfo": {"name": "fake-http", "version": "1.0"},
        })

    if method == "notifications/initialized":
        return None  # notifications carry no id -> no response

    if method == "tools/list":
        if MODE == "cache":
            global _tools_list_count
            _tools_list_count += 1
            if _tools_list_count == 1:
                return make_result(req, {"tools": [
                    {"name": "echo", "description": "Echo text back",
                     "inputSchema": {"type": "object",
                                     "properties": {"text": {"type": "string"}},
                                     "required": ["text"]}},
                ], "_meta": {"ttlMs": 60000, "cacheScope": "session"}})
            return make_error(req, -32603, "tools/list 应命中客户端缓存")
        return make_result(req, {"tools": [
            {"name": "echo", "description": "Echo text back",
             "inputSchema": {"type": "object",
                             "properties": {"text": {"type": "string"}},
                             "required": ["text"]}},
            {"name": "add", "description": "Add two numbers",
             "inputSchema": {"type": "object",
                             "properties": {"a": {"type": "number"},
                                            "b": {"type": "number"}},
                             "required": ["a", "b"]}},
        ]})

    if method == "tools/call":
        name = params.get("name", "")
        args = params.get("arguments", {}) or {}
        if name == "echo":
            return make_result(req, {
                "content": [{"type": "text",
                             "text": "echo: " + str(args.get("text", ""))}],
                "isError": False})
        if name == "add":
            total = int(args.get("a", 0)) + int(args.get("b", 0))
            return make_result(req, {
                "content": [{"type": "text", "text": str(total)}],
                "isError": False})
        if name == "fail":
            return make_result(req, {
                "content": [{"type": "text", "text": "boom"}],
                "isError": True})
        return make_error(req, -32602, "Unknown tool: " + name)

    if method == "resources/list":
        return make_result(req, {"resources": [
            {"uri": "file:///docs/readme.md", "name": "readme",
             "mimeType": "text/markdown", "description": "Project readme"},
        ]})

    if method == "resources/read":
        uri = params.get("uri", "")
        if uri == "file:///docs/readme.md":
            return make_result(req, {"contents": [
                {"uri": uri, "mimeType": "text/markdown",
                 "text": "# Fake Readme\nHello"}]})
        return make_result(req, {"contents": []})

    if method == "resources/templates/list":
        return make_result(req, {"resourceTemplates": [
            {"uriTemplate": "git:///{owner}/{repo}/blob/{sha}",
             "name": "blob", "description": "Git blob",
             "mimeType": "text/plain"},
        ]})

    if method == "prompts/list":
        return make_result(req, {"prompts": [
            {"name": "summarize",
             "description": "Summarize a document",
             "arguments": [
                 {"name": "topic", "description": "Topic", "required": True},
                 {"name": "lang", "description": "Language"},
             ]},
        ]})

    if method == "prompts/get":
        name = params.get("name", "")
        if name == "summarize":
            return make_result(req, {"messages": [
                {"role": "user",
                 "content": {"type": "text",
                             "text": "Summarize: " + str(params.get("arguments", {}).get("topic", ""))}},
            ]})
        return make_error(req, -32602, "Unknown prompt: " + name)

    return make_error(req, -32601, "Method not found: " + method)


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        try:
            req = json.loads(body)
        except json.JSONDecodeError:
            self.send_response(400)
            self.end_headers()
            return

        resp = handle(req)
        if resp is None:
            self.send_response(202)
            self.end_headers()
            return

        payload = json.dumps(resp)
        if MODE == "sse":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(("data: " + payload + "\n\n").encode())
        else:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            if MODE == "legacy":
                self.send_header("Mcp-Session-Id", "sess-123")
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
