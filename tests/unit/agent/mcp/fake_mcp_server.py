#!/usr/bin/env python3
"""Fake MCP stdio server for unit tests (Issue #27).

Speaks JSON-RPC 2.0 over stdio, one JSON object per line.
Modes (controlled by env FAKE_MCP_MODE):
  - "discover": respond to server/discover (2.0 stateless) [default]
  - "legacy":   reject server/discover with MethodNotFound, use initialize handshake
"""

import json
import os
import sys

MODE = os.environ.get("FAKE_MCP_MODE", "discover")


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def rpc_error(req, code, message):
    send({"jsonrpc": "2.0", "id": req.get("id"),
          "error": {"code": code, "message": message}})


def rpc_result(req, result):
    send({"jsonrpc": "2.0", "id": req.get("id"), "result": result})


def handle(req):
    method = req.get("method", "")
    params = req.get("params", {}) or {}

    if method == "server/discover":
        if MODE == "legacy":
            rpc_error(req, -32601, "Method not found")
        else:
            rpc_result(req, {"protocolVersion": "2026-07-28"})
        return

    if method == "initialize":
        rpc_result(req, {
            "protocolVersion": params.get("protocolVersion", "2025-11-25"),
            "capabilities": {},
            "serverInfo": {"name": "fake", "version": "1.0"},
        })
        return

    if method == "notifications/initialized":
        return  # notifications carry no id -> no response

    if method == "tools/list":
        rpc_result(req, {"tools": [
            {"name": "echo",
             "description": "Echo text back",
             "inputSchema": {"type": "object",
                             "properties": {"text": {"type": "string"}},
                             "required": ["text"]}},
            {"name": "add",
             "description": "Add two numbers",
             "inputSchema": {"type": "object",
                             "properties": {"a": {"type": "number"},
                                            "b": {"type": "number"}},
                             "required": ["a", "b"]}},
        ]})
        return

    if method == "tools/call":
        name = params.get("name", "")
        args = params.get("arguments", {}) or {}
        if name == "echo":
            rpc_result(req, {
                "content": [{"type": "text",
                             "text": "echo: " + str(args.get("text", ""))}],
                "isError": False,
            })
        elif name == "add":
            total = int(args.get("a", 0)) + int(args.get("b", 0))
            rpc_result(req, {
                "content": [{"type": "text", "text": str(total)}],
                "isError": False,
            })
        elif name == "fail":
            rpc_result(req, {
                "content": [{"type": "text", "text": "boom"}],
                "isError": True,
            })
        else:
            rpc_error(req, -32602, "Unknown tool: " + name)
        return

    if method == "resources/list":
        rpc_result(req, {"resources": [
            {"uri": "file:///docs/readme.md", "name": "readme",
             "mimeType": "text/markdown", "description": "Project readme"},
        ]})
        return

    if method == "resources/read":
        uri = params.get("uri", "")
        if uri == "file:///docs/readme.md":
            rpc_result(req, {"contents": [
                {"uri": uri, "mimeType": "text/markdown", "text": "# Fake Readme\nHello"},
            ]})
        else:
            rpc_result(req, {"contents": []})
        return

    rpc_error(req, -32601, "Method not found: " + method)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "id" not in req:
            continue
        handle(req)


if __name__ == "__main__":
    main()
