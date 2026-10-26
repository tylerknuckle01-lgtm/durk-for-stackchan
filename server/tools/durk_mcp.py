#!/usr/bin/env python3
"""Durk MCP server (stdio, no dependencies). Lets Claude Desktop / Claude Code / any MCP client
drive Durk's head, face, LEDs, camera and voice through the local xiaozhi server (:8003).

Config:  {"mcpServers": {"durk": {"command": "python", "args": ["C:\\\\path\\\\to\\\\durk-mcp\\\\durk_mcp.py"]}}}
Env DURK_URL overrides http://127.0.0.1:8003.
Design borrowed from kisaragi-mochi/stackchan-mcp (tool set) but reuses Durk's own server instead of a
second websocket server, so the robot keeps its normal voice brain.
"""
import json
import os
import sys
import urllib.request

BASE = os.environ.get("DURK_URL", "http://127.0.0.1:8003").rstrip("/")


def http(method, path, body=None, timeout=90):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read().decode())
        except Exception:
            return {"ok": False, "error": f"HTTP {e.code}"}
    except Exception as e:
        return {"ok": False, "error": str(e)}


def dev(name, args=None):
    return http("POST", "/durk/tool", {"name": name, "args": args or {}})


S = lambda **p: {"type": "object", "properties": p, "required": [k for k, v in p.items() if not v.pop("_opt", False)]}
TOOLS = {
    "say": ("Make Durk speak out loud (in his own voice).",
            S(text={"type": "string"}), lambda a: http("POST", "/durk/say", {"text": a["text"]})),
    "look": ("Turn Durk's head: left, right, up, down, center (left/right from the viewer's side).",
             S(direction={"type": "string", "enum": ["left", "right", "up", "down", "center"]}),
             lambda a: dev("robot.look", {"direction": a["direction"]})),
    "set_head_angles": ("Set head angles. yaw -60..60 (negative = viewer's left), pitch 0..30 (0 level, + up).",
                        S(yaw={"type": "integer"}, pitch={"type": "integer"}),
                        lambda a: dev("robot.set_head_angles", {"yaw": a["yaw"], "pitch": a["pitch"]})),
    "gesture": ("Smooth expressive head gesture.",
                S(name={"type": "string", "enum": ["nod", "shake", "tilt", "happy", "sad", "think", "surprise", "dizzy", "bow", "sleepy", "love"]}),
                lambda a: dev("robot.gesture", {"name": a["name"]})),
    "see": ("Take a photo with Durk's camera and answer a question about it.",
            S(question={"type": "string"}), lambda a: dev("self.camera.take_photo", {"question": a["question"]})),
    "set_led_color": ("Set Durk's 12 body LEDs (r,g,b 0-255).",
                      S(r={"type": "integer"}, g={"type": "integer"}, b={"type": "integer"}),
                      lambda a: dev("robot.set_led_color", {"r": a["r"], "g": a["g"], "b": a["b"], "index": -1})),
    "trigger": ("Fire a smart-home style event (doorbell, arriving, goodnight...) with optional custom text.",
                S(event={"type": "string"}, text={"type": "string", "_opt": True}),
                lambda a: http("POST", "/durk/trigger", a)),
    "list_device_tools": ("List every tool the robot firmware exposes.", S(), lambda a: http("GET", "/durk/tools")),
    "call_device_tool": ("Call any robot firmware tool by name with JSON args (see list_device_tools).",
                         S(name={"type": "string"}, args={"type": "object", "_opt": True}),
                         lambda a: dev(a["name"], a.get("args") or {})),
}


def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        mid, method, params = req.get("id"), req.get("method"), req.get("params") or {}
        if mid is None:
            continue  # notification
        if method == "initialize":
            res = {"protocolVersion": params.get("protocolVersion", "2025-06-18"),
                   "capabilities": {"tools": {}}, "serverInfo": {"name": "durk", "version": "1.0"}}
        elif method == "tools/list":
            res = {"tools": [{"name": n, "description": d, "inputSchema": s} for n, (d, s, _) in TOOLS.items()]}
        elif method == "tools/call":
            n = params.get("name")
            if n not in TOOLS:
                send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": f"unknown tool {n}"}})
                continue
            out = TOOLS[n][2](params.get("arguments") or {})
            res = {"content": [{"type": "text", "text": json.dumps(out, ensure_ascii=False)}],
                   "isError": not out.get("ok", True)}
        elif method == "ping":
            res = {}
        else:
            send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}})
            continue
        send({"jsonrpc": "2.0", "id": mid, "result": res})


if __name__ == "__main__":
    main()
