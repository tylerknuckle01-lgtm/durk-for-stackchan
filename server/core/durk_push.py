"""Durk: server-initiated speech, reminders and the morning rundown.

HTTP (on the existing 8003 server):
  GET  /durk/pending            -> {"pending": bool}   (polled by the robot)
  POST /durk/say      {"text": "...", "llm": false}     speak text (llm=true lets Grok phrase it)
  POST /durk/remind   {"time": "15:00" | "2026-10-15T15:00" | "in 10 minutes", "message": "..."}
  GET  /durk/reminders                                 list pending reminders
  POST /durk/rundown                                   run the morning rundown now
Settings live in data/durk.json, reminders in data/durk_reminders.json.
"""
import asyncio
import json
import os
import re
import threading
import time
import uuid
from datetime import datetime, timedelta

from aiohttp import web
from config.logger import setup_logging

try:
    from zoneinfo import ZoneInfo
    TZ = ZoneInfo(os.environ.get("DURK_TZ", "UTC"))
except Exception:  # tzdata missing
    TZ = None

TAG = __name__
logger = setup_logging()

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SETTINGS_FILE = os.path.join(BASE, "data", "durk.json")
REMINDERS_FILE = os.path.join(BASE, "data", "durk_reminders.json")
DEFAULT_SETTINGS = {
    "rundown_enabled": True,
    "rundown_time": "07:00",
    "rundown_days": ["mon", "tue", "wed", "thu", "fri"],
    "rundown_location": os.environ.get("DURK_LOCATION", "New York, NY"),
}
DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]

_lock = threading.Lock()
_pending = []          # list of {"text": str, "ts": float}
_connections = set()   # live ConnectionHandler objects
_config = None


def now():
    return datetime.now(TZ) if TZ else datetime.now()


# ---------------- settings / storage ----------------
def load_settings():
    s = dict(DEFAULT_SETTINGS)
    try:
        with open(SETTINGS_FILE, "r", encoding="utf-8") as f:
            s.update(json.load(f))
    except FileNotFoundError:
        save_json(SETTINGS_FILE, s)
    except Exception as e:
        logger.bind(tag=TAG).error(f"durk.json unreadable, using defaults: {e}")
    return s


def save_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, path)


def load_reminders():
    try:
        with open(REMINDERS_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return []
    except Exception as e:
        logger.bind(tag=TAG).error(f"reminders file unreadable: {e}")
        return []


def parse_time(text):
    """Return an aware datetime (local DURK_TZ) for a spoken/ISO time, or None."""
    t = (text or "").strip().lower()
    n = now()
    m = re.match(r"^in\s+(\d+(?:\.\d+)?)\s*(second|sec|minute|min|hour|hr)s?$", t)
    if m:
        v = float(m.group(1))
        unit = m.group(2)
        secs = v * (1 if unit.startswith("s") else 60 if unit.startswith("m") else 3600)
        return n + timedelta(seconds=secs)
    try:
        d = datetime.fromisoformat(t.upper().replace(" ", "T") if re.match(r"^\d{4}-", t) else t)
        if d.tzinfo is None and TZ:
            d = d.replace(tzinfo=TZ)
        return d
    except Exception:
        pass
    m = re.match(r"^(\d{1,2})(?::(\d{2}))?\s*(a\.?m\.?|p\.?m\.?)?$", t)
    if m:
        h = int(m.group(1)); mi = int(m.group(2) or 0); ap = m.group(3)
        if ap:
            ap = ap[0]
            if h == 12:
                h = 0
            if ap == "p":
                h += 12
        if h > 23 or mi > 59:
            return None
        cand = n.replace(hour=h, minute=mi, second=0, microsecond=0)
        if not ap and h < 12:
            # ambiguous "3" -> next occurrence of 3:00 am/pm
            if cand <= n:
                pm = cand + timedelta(hours=12)
                cand = pm if pm > n else cand + timedelta(days=1)
        elif cand <= n:
            cand += timedelta(days=1)
        return cand
    return None


def add_reminder(time_text, message):
    when = parse_time(time_text)
    if not when:
        return None
    r = {"id": uuid.uuid4().hex[:8], "due": when.isoformat(), "message": message.strip(), "created": now().isoformat()}
    with _lock:
        rs = load_reminders()
        rs.append(r)
        save_json(REMINDERS_FILE, rs)
    logger.bind(tag=TAG).info(f"Durk reminder set for {when.isoformat()}: {message}")
    return when


def speakable_time(when):
    s = when.strftime("%I:%M %p").lstrip("0")
    if when.date() != now().date():
        s += " " + ("tomorrow" if when.date() == (now() + timedelta(days=1)).date() else when.strftime("on %A, %B %d"))
    return s


# ---------------- connections / delivery ----------------
def register(conn):
    _connections.add(conn)


def unregister(conn):
    _connections.discard(conn)


def has_pending():
    with _lock:
        return bool(_pending)


def say(text):
    """Queue text to be spoken. Delivered immediately if a session is open, otherwise
    the robot picks it up on its next poll (every ~5 s) and wakes itself."""
    text = (text or "").strip()
    if not text:
        return
    with _lock:
        _pending.append({"text": text, "ts": time.time()})
    logger.bind(tag=TAG).info(f"Durk say queued: {text}")
    for conn in list(_connections):
        try:
            ws_state = getattr(getattr(getattr(conn, "websocket", None), "state", None), "name", "")
            if ws_state == "OPEN" and getattr(conn, "tts", None) and not conn.client_is_speaking and conn.loop:
                asyncio.run_coroutine_threadsafe(deliver(conn), conn.loop)
                break
        except Exception as e:
            logger.bind(tag=TAG).warning(f"direct delivery failed: {e}")


async def deliver(conn):
    """Speak everything queued over this connection."""
    for _ in range(50):
        if getattr(conn, "tts", None):
            break
        await asyncio.sleep(0.1)
    with _lock:
        items = [p for p in _pending if time.time() - p["ts"] < 6 * 3600]
        _pending.clear()
    if not items or not conn.tts:
        logger.bind(tag=TAG).info("Durk push: nothing to say")
        return False
    text = " ".join(p["text"] for p in items)
    from core.handle.intentHandler import speak_txt
    conn.client_abort = False
    conn.sentence_id = uuid.uuid4().hex
    # The device only enters Speaking (and enables its speaker) on tts:start,
    # which the normal flow sends from send_stt_message. Send it ourselves.
    from core.handle.sendAudioHandle import send_tts_message
    await send_tts_message(conn, "start")
    speak_txt(conn, text)
    logger.bind(tag=TAG).info(f"Durk push: speaking to device: {text}")
    return True


# ---------------- LLM phrasing ----------------
def _llm_complete(system, user):
    llm_name = _config["selected_module"]["LLM"]
    c = _config["LLM"][llm_name]
    from openai import OpenAI
    client = OpenAI(api_key=c["api_key"], base_url=c.get("url") or c.get("base_url"))
    r = client.chat.completions.create(
        model=c["model_name"], temperature=0.7, max_tokens=300,
        messages=[{"role": "system", "content": system}, {"role": "user", "content": user}])
    return r.choices[0].message.content.strip()


PERSONA = ("You are Durk, the owner's small desk robot (a Stack-chan). Your words will be read aloud "
           "by text-to-speech, so write plain spoken English only: no emoji, no markdown, no lists, US units. "
           "Be warm, upbeat and brief.")


async def llm_say(instruction):
    try:
        text = await asyncio.get_running_loop().run_in_executor(None, _llm_complete, PERSONA, instruction)
    except Exception as e:
        logger.bind(tag=TAG).error(f"Durk LLM phrasing failed: {e}")
        text = None
    return text


async def build_rundown():
    s = load_settings()
    n = now()
    weather = None
    try:
        from plugins_func.functions.get_weather import build_report
        weather = await build_report(s.get("rundown_location") or os.environ.get("DURK_LOCATION", "New York, NY"))
    except Exception as e:
        logger.bind(tag=TAG).error(f"rundown weather failed: {e}")
    today_rem = [r for r in load_reminders() if datetime.fromisoformat(r["due"]).date() == n.date()]
    rem_txt = "; ".join(f"{speakable_time(datetime.fromisoformat(r['due']))}: {r['message']}" for r in today_rem) or "none"
    try:
        from core import durk_google
        work = await asyncio.get_running_loop().run_in_executor(None, durk_google.rundown_section)
    except Exception as e:
        logger.bind(tag=TAG).error(f"rundown google failed: {e}")
        work = "Calendar and jobs unavailable (skip this topic)."
    instr = (f"Give the owner their good-morning rundown in 4 to 7 short sentences. Today is "
             f"{n.strftime('%A, %B')} {n.day}, {n.year}, and it's {speakable_time(n)}.\n"
             f"Weather data:\n{weather or 'Weather is unavailable right now.'}\n"
             f"Reminders set for today: {rem_txt}.\n"
             f"Calendar and jobs:\n{work}\n"
             "Mention the date, current temperature and today's high/low and conditions in Fahrenheit, "
             "any reminders, today's calendar events, a quick count of jobs due today and overdue (name at most two), and end with a short encouraging line.")
    text = await llm_say(instr)
    if not text:
        text = f"Good morning! It's {n.strftime('%A, %B')} {n.day}. I couldn't reach my brain for the full rundown, sorry."
    return text


# ---------------- scheduler ----------------
async def scheduler_loop():
    last_rundown_date = None
    logger.bind(tag=TAG).info(f"Durk scheduler started (settings: {load_settings()})")
    while True:
        try:
            n = now()
            s = load_settings()
            if s.get("rundown_enabled"):
                hh, mm = [int(x) for x in s.get("rundown_time", "07:00").split(":")]
                target = n.replace(hour=hh, minute=mm, second=0, microsecond=0)
                if (DAYS[n.weekday()] in [d.lower()[:3] for d in s.get("rundown_days", [])]
                        and target <= n < target + timedelta(minutes=10)
                        and last_rundown_date != n.date()):
                    last_rundown_date = n.date()
                    logger.bind(tag=TAG).info("Durk morning rundown firing")
                    say(await build_rundown())
            due = []
            with _lock:
                rs = load_reminders()
                keep = []
                for r in rs:
                    (due if datetime.fromisoformat(r["due"]) <= n else keep).append(r)
                if due:
                    save_json(REMINDERS_FILE, keep)
            for r in due:
                late = (n - datetime.fromisoformat(r["due"])).total_seconds() > 300
                prefix = "Hey, I missed this one earlier while I was offline. " if late else "Hey, reminder: "
                logger.bind(tag=TAG).info(f"Durk reminder firing: {r['message']}")
                say(prefix + r["message"])
        except Exception as e:
            logger.bind(tag=TAG).error(f"Durk scheduler error: {e}")
        await asyncio.sleep(15)


# ---------------- HTTP ----------------
async def _json(request):
    try:
        return await request.json()
    except Exception:
        return {}


async def h_pending(request):
    return web.json_response({"pending": has_pending()})


async def h_say(request):
    body = await _json(request)
    text = (body.get("text") or "").strip()
    if not text:
        return web.json_response({"ok": False, "error": "text required"}, status=400)
    if body.get("llm"):
        text = await llm_say(f"Say this to the owner in your own words, briefly: {text}") or text
    say(text)
    return web.json_response({"ok": True, "text": text, "live_session": bool(_connections)})


async def h_remind(request):
    body = await _json(request)
    when = add_reminder(body.get("time", ""), body.get("message", ""))
    if not when or not (body.get("message") or "").strip():
        return web.json_response({"ok": False, "error": "need time and message"}, status=400)
    return web.json_response({"ok": True, "due": when.isoformat()})


async def h_reminders(request):
    return web.json_response({"reminders": load_reminders(), "settings": load_settings()})


async def h_rundown(request):
    text = await build_rundown()
    say(text)
    return web.json_response({"ok": True, "text": text})


# ---------------- static assets for the device SD card ----------------
_ASSET_DIRS = {"sfx": (".ogg",), "faces": (".json", ".bin")}


async def h_asset(request):
    kind, name = request.match_info["kind"], request.match_info["name"]
    if kind not in _ASSET_DIRS:
        raise web.HTTPNotFound()
    d = os.path.join(BASE, "data", kind)
    if name == "index.txt":
        files = sorted(f for f in os.listdir(d) if f.lower().endswith(_ASSET_DIRS[kind])) if os.path.isdir(d) else []
        return web.Response(text="\n".join(files) + "\n")
    if "/" in name or "\\" in name or ".." in name or not name.lower().endswith(_ASSET_DIRS[kind]):
        raise web.HTTPNotFound()
    path = os.path.join(d, name)
    if not os.path.isfile(path):
        raise web.HTTPNotFound()
    return web.FileResponse(path)

def setup(app, config):
    global _config
    _config = config
    app.add_routes([
        web.get("/durk/{kind}/{name}", h_asset),
        web.get("/durk/pending", h_pending),
        web.post("/durk/say", h_say),
        web.post("/durk/remind", h_remind),
        web.get("/durk/reminders", h_reminders),
        web.post("/durk/rundown", h_rundown),
    ])
    asyncio.get_event_loop().create_task(scheduler_loop())
