"""Durk bridge (additive): lets outside tools drive the robot.

  GET  /durk/tools                 -> device MCP tools (needs/opens a live session)
  POST /durk/tool   {name, args}   -> call a device MCP tool, e.g. robot.look / robot.gesture / self.camera.take_photo
  POST /durk/trigger {event, text?, gesture?, emotion?, llm?}  -> smart-home style event: speak + react
  GET  /durk/bridge_status

Used by tools/durk_mcp.py (MCP stdio server for Claude Desktop / Claude Code etc.) and Home Assistant.
Wakes the robot silently when no session is open by making /durk/pending report true once.
"""
import asyncio
import json
import time

from aiohttp import web

from config.logger import setup_logging
from core import durk_push

TAG = __name__
logger = setup_logging()

_wake_until = 0.0
_orig_has_pending = durk_push.has_pending

EVENT_TEXT = {
    "doorbell": "Ding dong! Someone's at the front door.",
    "motion": "Heads up, I noticed motion.",
    "garage_open": "Just so you know, the garage door is open.",
    "washer_done": "The laundry is done.",
    "package": "Looks like a package was delivered.",
    "leaving": "Have a good one. See you soon.",
    "arriving": "Welcome back!",
    "goodnight": "Good night.",
}
EVENT_GESTURE = {"doorbell": "surprise", "motion": "think", "arriving": "happy", "goodnight": "sleepy",
                 "leaving": "nod", "package": "happy", "washer_done": "nod", "garage_open": "tilt"}


def _has_pending_patched():
    return _orig_has_pending() or time.time() < _wake_until


def _live_conn():
    for conn in list(durk_push._connections):
        try:
            ws_state = getattr(getattr(getattr(conn, "websocket", None), "state", None), "name", "")
            mc = getattr(conn, "mcp_client", None)
            if ws_state == "OPEN" and mc is not None and conn.loop:
                return conn
        except Exception:
            pass
    return None


async def _get_conn(timeout=40):
    """Return a live connection with device MCP ready; wake the robot if needed."""
    global _wake_until
    conn = _live_conn()
    if conn is None:
        _wake_until = time.time() + timeout
        logger.bind(tag=TAG).info("bridge: waking robot for tool call")
    end = time.time() + timeout
    while time.time() < end:
        conn = _live_conn()
        if conn is not None:
            try:
                if await conn.mcp_client.is_ready():
                    _wake_until = 0
                    return conn
            except Exception:
                pass
        await asyncio.sleep(0.3)
    _wake_until = 0
    return None


async def _call_tool(name, args):
    from core.providers.tools.device_mcp.mcp_handler import call_mcp_tool
    from core.utils.util import sanitize_tool_name
    conn = await _get_conn()
    if conn is None:
        raise RuntimeError("robot not connected (is Durk powered and on Wi-Fi?)")
    sname = sanitize_tool_name(name)
    if not conn.mcp_client.has_tool(sname):
        raise RuntimeError(f"unknown device tool {name}")
    coro = call_mcp_tool(conn, conn.mcp_client, sname, json.dumps(args or {}), 60)
    if asyncio.get_running_loop() is conn.loop:
        return await coro
    return await asyncio.wrap_future(asyncio.run_coroutine_threadsafe(coro, conn.loop))


async def _json(request):
    try:
        return await request.json()
    except Exception:
        return {}


async def h_tools(request):
    conn = await _get_conn()
    if conn is None:
        return web.json_response({"ok": False, "error": "robot not connected"}, status=503)
    tools = [{"name": t.get("name"), "description": t.get("description", "")[:300],
              "inputSchema": t.get("inputSchema", {})} for t in conn.mcp_client.tools.values()]
    return web.json_response({"ok": True, "tools": tools})


async def h_tool(request):
    body = await _json(request)
    name = (body.get("name") or "").strip()
    if not name:
        return web.json_response({"ok": False, "error": "name required"}, status=400)
    try:
        res = await _call_tool(name, body.get("args") or {})
        logger.bind(tag=TAG).info(f"bridge tool {name} -> {str(res)[:200]}")
        return web.json_response({"ok": True, "result": res if isinstance(res, (str, dict, list, bool, int, float)) else str(res)})
    except Exception as e:
        logger.bind(tag=TAG).warning(f"bridge tool {name} failed: {e}")
        return web.json_response({"ok": False, "error": str(e)}, status=502)


async def h_trigger(request):
    body = await _json(request)
    event = (body.get("event") or "").strip().lower()
    text = (body.get("text") or "").strip() or EVENT_TEXT.get(event, "")
    if not event and not text:
        return web.json_response({"ok": False, "error": "event or text required"}, status=400)
    if not text:
        text = f"Heads up: {event.replace('_', ' ')}."
    if body.get("llm"):
        text = await durk_push.llm_say(f"A smart-home event happened ({event}). Tell the owner briefly: {text}") or text
    gesture = body.get("gesture") or EVENT_GESTURE.get(event)
    logger.bind(tag=TAG).info(f"trigger {event}: {text}")
    durk_push.say(text)

    async def _react():
        await asyncio.sleep(2)
        try:
            if gesture:
                await _call_tool("robot.gesture", {"name": gesture})
        except Exception as e:
            logger.bind(tag=TAG).info(f"trigger gesture skipped: {e}")
    asyncio.ensure_future(_react())
    return web.json_response({"ok": True, "event": event, "text": text, "gesture": gesture})


async def h_status(request):
    return web.json_response({"ok": True, "live_session": _live_conn() is not None,
                              "events": sorted(EVENT_TEXT)})


def setup(app, config):
    durk_push.has_pending = _has_pending_patched
    app.add_routes([
        web.get("/durk/tools", h_tools),
        web.post("/durk/tool", h_tool),
        web.post("/durk/trigger", h_trigger),
        web.get("/durk/bridge_status", h_status),
    ])
    logger.bind(tag=TAG).info("Durk bridge routes ready")
    try:
        from core import durk_nfc
        durk_nfc.setup(app)
        logger.bind(tag=TAG).info("Durk NFC route ready (/durk/nfc)")
    except Exception as e:
        logger.bind(tag=TAG).warning(f"nfc skipped: {e}")
    try:
        from core import durk_tools_filter
        durk_tools_filter.install()
        logger.bind(tag=TAG).info(f"LLM tool filter on: hiding {sorted(durk_tools_filter.HIDDEN)}")
    except Exception as e:
        logger.bind(tag=TAG).warning(f"tool filter skipped: {e}")
    try:
        from core import durk_timing
        durk_timing.install()
    except Exception as e:
        logger.bind(tag=TAG).warning(f"timing hooks skipped: {e}")
    try:
        from core import durk_modes
        durk_modes.install()
        logger.bind(tag=TAG).info(f"Durk modes installed (unhinged, kid) state={durk_modes.state()}")
    except Exception as e:
        logger.bind(tag=TAG).warning(f"modes skipped: {e}")
    try:
        from core import durk_memory
        durk_memory.install()
        logger.bind(tag=TAG).info("Durk memory installed (PC + SD card sync)")
    except Exception as e:
        logger.bind(tag=TAG).warning(f"memory skipped: {e}")
    try:
        from core import durk_slow
        durk_slow.install()
    except Exception as e:
        logger.bind(tag=TAG).warning(f"slow-reply guard skipped: {e}")
