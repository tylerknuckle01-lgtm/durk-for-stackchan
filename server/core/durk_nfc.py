"""Durk NFC cards: the robot POSTs {"uid": "..."} to /durk/nfc when a card is tapped on his body.
Mapping lives in data/nfc_cards.json:
  {"cards": {"04A1B2C3": {"label": "kitchen card", "action": "say|rundown|unhinged|theme|gesture|trigger",
                          "value": "..."}},
   "last_unknown": {"uid": "...", "at": "..."}}
Unknown cards: Durk asks what the card should do; the LLM tool assign_nfc_card maps the last unknown card."""
import asyncio
import datetime
import json
import os
import threading

from aiohttp import web
from config.logger import setup_logging

TAG = __name__
logger = setup_logging()
BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILE = os.path.join(BASE, "data", "nfc_cards.json")
ACTIONS = ("say", "rundown", "unhinged", "theme", "gesture", "trigger")
_lock = threading.Lock()


def load():
    try:
        with open(FILE, "r", encoding="utf-8") as f:
            d = json.load(f)
        if isinstance(d, dict) and isinstance(d.get("cards"), dict):
            return d
    except Exception:
        pass
    return {"cards": {}, "last_unknown": None}


def save(d):
    with _lock:
        tmp = FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2, ensure_ascii=False)
        os.replace(tmp, FILE)


def assign(action, value="", label="", uid=""):
    action = (action or "").strip().lower()
    if action not in ACTIONS:
        return f"Unknown action. Use one of: {', '.join(ACTIONS)}."
    d = load()
    uid = (uid or (d.get("last_unknown") or {}).get("uid") or "").upper()
    if not uid:
        return "No new card has been tapped yet. Ask the owner to tap the card first."
    d["cards"][uid] = {"label": label or f"card {uid[-4:]}", "action": action, "value": value or ""}
    if (d.get("last_unknown") or {}).get("uid") == uid:
        d["last_unknown"] = None
    save(d)
    logger.bind(tag=TAG).info(f"nfc card {uid} assigned: {action} {value!r} ({label})")
    return f"Card saved: tapping it will now do '{action}' {value}. Confirm briefly."


async def _run(uid, card):
    from core import durk_push, durk_bridge
    action, value = card.get("action"), (card.get("value") or "").strip()
    try:
        if action == "say":
            durk_push.say(value or "Beep boop.")
        elif action == "rundown":
            durk_push.say(await durk_push.build_rundown())
        elif action == "unhinged":
            from core import durk_modes
            on = value.lower() not in ("off", "false", "0", "no", "calm")
            durk_modes.set_unhinged(on)
            durk_push.say("Unhinged mode on. Buckle up." if on else "Okay, back to normal mode.")
        elif action == "theme":
            await durk_bridge._call_tool("robot.set_face_theme", {"name": value or "classic"})
        elif action == "gesture":
            await durk_bridge._call_tool("robot.gesture", {"name": value or "happy"})
        elif action == "trigger":
            text = durk_bridge.EVENT_TEXT.get(value.lower(), f"Heads up: {value}.")
            durk_push.say(text)
            g = durk_bridge.EVENT_GESTURE.get(value.lower())
            if g:
                await asyncio.sleep(2)
                await durk_bridge._call_tool("robot.gesture", {"name": g})
        logger.bind(tag=TAG).info(f"nfc card {uid} -> {action} {value!r} done")
    except Exception as e:
        logger.bind(tag=TAG).warning(f"nfc card {uid} action {action} failed: {e}")


async def h_nfc(request):
    try:
        body = await request.json()
    except Exception:
        body = {}
    uid = "".join(ch for ch in str(body.get("uid", "")).upper() if ch in "0123456789ABCDEF")[:20]
    if not uid:
        return web.json_response({"ok": False, "error": "uid required"}, status=400)
    d = load()
    card = d["cards"].get(uid)
    if card:
        logger.bind(tag=TAG).info(f"nfc tap {uid} ({card.get('label')})")
        asyncio.ensure_future(_run(uid, card))
        return web.json_response({"ok": True, "known": True, "action": card.get("action")})
    d["last_unknown"] = {"uid": uid, "at": datetime.datetime.now().isoformat(timespec="seconds")}
    save(d)
    logger.bind(tag=TAG).info(f"nfc tap {uid} (unknown card)")
    from core import durk_push
    durk_push.say("New card! What should this card do? You can have me say something, run your morning rundown, "
                  "switch unhinged mode, change my face, do a gesture, or a smart-home alert.")
    return web.json_response({"ok": True, "known": False})


async def h_cards(request):
    return web.json_response(load())


def setup(app):
    app.add_routes([web.post("/durk/nfc", h_nfc), web.get("/durk/nfc_cards", h_cards)])
