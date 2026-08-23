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

# cache 模式：tools/list 首次返回 _meta.ttlMs，后续返回错误（验证客户端缓存）
_tools_list_count = 0


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
        if MODE == "cache":
            global _tools_list_count
            _tools_list_count += 1
            if _tools_list_count == 1:
                rpc_result(req, {"tools": [
                    {"name": "echo",
                     "description": "Echo text back",
                     "inputSchema": {"type": "object",
                                     "properties": {"text": {"type": "string"}},
                                     "required": ["text"]}},
                ], "_meta": {"ttlMs": 60000, "cacheScope": "session"}})
            else:
                rpc_error(req, -32603, "tools/list 应命中客户端缓存")
            return
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

    if method == "resources/templates/list":
        rpc_result(req, {"resourceTemplates": [
            {"uriTemplate": "git:///{owner}/{repo}/blob/{sha}",
             "name": "blob", "description": "Git blob",
             "mimeType": "text/plain"},
        ]})
        return

    if method == "prompts/list":
        rpc_result(req, {"prompts": [
            {"name": "summarize",
             "description": "Summarize a document",
             "arguments": [
                 {"name": "topic", "description": "Topic", "required": True},
                 {"name": "lang", "description": "Language"},
             ]},
        ]})
        return

    if method == "prompts/get":
        name = params.get("name", "")
        if name == "summarize":
            rpc_result(req, {"messages": [
                {"role": "user",
                 "content": {"type": "text",
                             "text": "Summarize: " + str(params.get("arguments", {}).get("topic", ""))}},
            ]})
        else:
            rpc_error(req, -32602, "Unknown prompt: " + name)
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
