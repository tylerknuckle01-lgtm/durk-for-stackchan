"""Durk personality memory (nicknames, jokes, facts remembered between sessions).

PC copy:   data/durk_memory.json (server cache + source for restores)
Card copy: /sdcard/memory/durk_memory.json on the robot (via hidden device tools robot.memory_*)
Backups:   data/memory_backups/ - a versioned copy after every change plus a daily snapshot;
           keeps the newest 30 copies and one per day for 30 days.
Sync:      whenever Durk is connected (never wakes him): card missing/empty/corrupt -> restored from PC;
           PC empty but card has facts -> PC restored from card; otherwise PC wins and is pushed.
Kid mode:  nothing is saved and memory is not added to the prompt. Swearing is never saved.
"""
import asyncio
import datetime
import glob
import json
import os
import re
import threading

from config.logger import setup_logging

TAG = __name__
logger = setup_logging()
BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILE = os.path.join(BASE, "data", "durk_memory.json")
BACKUP_DIR = os.path.join(BASE, "data", "memory_backups")
CAP = 15 * 1024          # stay under the firmware's 16 KB cap
MAX_FACT = 200
KEEP_RECENT, KEEP_DAYS = 30, 30
_lock = threading.Lock()
_state = {"dirty": False, "synced_conn": None, "daily": None}
_BAD = re.compile(r"\b(fuck\w*|shit\w*|bitch\w*|cunt\w*|asshole\w*|dick\w*|piss\w*|bastard\w*|damn\w*)\b", re.I)


def _now():
    return datetime.datetime.now()


def _empty():
    return {"v": 1, "updated": "", "facts": []}


def _valid(d):
    return isinstance(d, dict) and isinstance(d.get("facts"), list)


def _dump(d):
    return json.dumps(d, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def load():
    try:
        with open(FILE, "r", encoding="utf-8") as f:
            d = json.load(f)
        return d if _valid(d) else _empty()
    except Exception:
        return _empty()


def _backup(text, tag):
    os.makedirs(BACKUP_DIR, exist_ok=True)
    name = f"durk_memory-{_now():%Y%m%d-%H%M%S%f}-{tag}.json"
    with open(os.path.join(BACKUP_DIR, name), "w", encoding="utf-8") as f:
        f.write(text)
    _rotate()


def _rotate():
    files = sorted(glob.glob(os.path.join(BACKUP_DIR, "durk_memory-*.json")), reverse=True)
    keep = set(files[:KEEP_RECENT])
    days = {}
    for p in files:  # newest first -> first seen per day is that day's newest
        day = os.path.basename(p)[12:20]
        days.setdefault(day, p)
    for day in sorted(days, reverse=True)[:KEEP_DAYS]:
        keep.add(days[day])
    for p in files:
        if p not in keep:
            try:
                os.remove(p)
            except Exception:
                pass


def save(d, tag="change"):
    d["updated"] = _now().isoformat(timespec="seconds")
    text = _dump(d)
    with _lock:
        tmp = FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, FILE)
        _backup(text, tag)
        _state["dirty"] = True
    return d


def _kid():
    try:
        from core import durk_modes
        return durk_modes.state()[1]
    except Exception:
        return False


def remember(text):
    text = " ".join((text or "").split())[:MAX_FACT]
    if not text:
        return "Nothing to remember."
    if _kid():
        return "Kid mode is on, so nothing new is saved to memory. Tell the owner briefly."
    if _BAD.search(text):
        return "Not saved: memories can't contain swearing. Offer a clean version."
    d = load()
    if any(f.get("t", "").lower() == text.lower() for f in d["facts"]):
        return "Already remembered."
    d["facts"].append({"t": text, "at": _now().strftime("%Y-%m-%d")})
    while len(_dump(d)) > CAP and len(d["facts"]) > 1:
        d["facts"].pop(0)  # drop oldest
    save(d)
    return f"Saved to memory: {text}. Confirm in one short line."


def forget(query):
    q = (query or "").strip().lower()
    if not q:
        return "Say what to forget."
    d = load()
    if q in ("everything", "all", "all of it"):
        gone = [f["t"] for f in d["facts"]]
        d["facts"] = []
    else:
        gone = [f["t"] for f in d["facts"] if q in f.get("t", "").lower()]
        d["facts"] = [f for f in d["facts"] if q not in f.get("t", "").lower()]
    if not gone:
        return "Nothing in memory matched that."
    save(d, "forget")
    return f"Forgot {len(gone)} item(s): " + "; ".join(gone)[:300]


def prompt_block():
    if _kid():
        return ""
    facts = load()["facts"]
    if not facts:
        return ""
    lines = "\n".join(f"- {f['t']}" for f in facts[-60:])
    return ("\n\n## THINGS YOU REMEMBER (saved memory)\n" + lines +
            "\nUse these naturally when relevant (nicknames, running jokes). To save something new call "
            "remember_fact; to delete call forget_fact.")


def _apply(dialogue):
    try:
        block = prompt_block()
    except Exception:
        block = ""
    if not block or not dialogue:
        return dialogue
    out = list(dialogue)
    for i, m in enumerate(out):
        if isinstance(m, dict) and m.get("role") == "system":
            m = dict(m)
            m["content"] = (m.get("content") or "") + block
            out[i] = m
            return out
    return out


# ---------------- card sync ----------------
async def _dev(conn, name, args=None):
    from core.providers.tools.device_mcp.mcp_handler import call_mcp_tool
    coro = call_mcp_tool(conn, conn.mcp_client, name, json.dumps(args or {}), 30)
    if asyncio.get_running_loop() is conn.loop:
        res = await coro
    else:
        res = await asyncio.wrap_future(asyncio.run_coroutine_threadsafe(coro, conn.loop))
    return json.loads(res) if isinstance(res, str) else res


async def sync(conn, reason="connect"):
    if not conn.mcp_client.has_tool("robot_memory_read"):
        return "firmware without memory tools"
    r = await _dev(conn, "robot_memory_read")
    if not r.get("ok"):
        return f"card unavailable: {r.get('error')}"
    card = None
    try:
        card = json.loads(r.get("data") or "")
        if not _valid(card):
            card = None
    except Exception:
        card = None
    pc = load()
    if not pc["facts"] and card and card["facts"]:
        with _lock:
            text = _dump(card)
            tmp = FILE + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(text)
            os.replace(tmp, FILE)
            _backup(text, "from-card")
        _state["dirty"] = False
        return f"PC memory restored from card ({len(card['facts'])} facts)"
    if card is not None and _dump(card) == _dump(pc):
        _state["dirty"] = False
        return "in sync"
    if not pc["facts"] and card is None and not r.get("exists"):
        _state["dirty"] = False
        return "nothing to sync"
    w = await _dev(conn, "robot_memory_write", {"data": _dump(pc)})
    if not w.get("ok"):
        return f"card write failed: {w.get('error')}"
    _state["dirty"] = False
    return f"card updated from PC ({len(pc['facts'])} facts, {'new/blank card restored' if card is None else reason})"


async def _loop():
    from core import durk_bridge
    while True:
        await asyncio.sleep(15)
        try:
            now = _now()
            if now.hour >= 3 and _state["daily"] != now.date():
                _state["daily"] = now.date()
                if os.path.exists(FILE):
                    with _lock:
                        with open(FILE, "r", encoding="utf-8") as f:
                            _backup(f.read(), "daily")
                _state["synced_conn"] = None  # pull/compare again on the next live session
            conn = durk_bridge._live_conn()  # never wakes the robot
            if conn is None:
                continue
            key = id(conn)
            if _state["synced_conn"] == key and not _state["dirty"]:
                continue
            if not await conn.mcp_client.is_ready():
                continue
            msg = await sync(conn, "dirty" if _state["dirty"] else "connect")
            _state["synced_conn"] = key
            logger.bind(tag=TAG).info(f"memory sync: {msg}")
        except Exception as e:
            logger.bind(tag=TAG).warning(f"memory sync skipped: {e}")


def install():
    from core.providers.llm.openai import openai as O
    for name in ("response", "response_with_functions"):
        orig = getattr(O.LLMProvider, name)

        def make(orig):
            def gen(self, session_id, dialogue, *a, **kw):
                yield from orig(self, session_id, _apply(dialogue), *a, **kw)
            return gen
        setattr(O.LLMProvider, name, make(orig))
    asyncio.get_event_loop().create_task(_loop())
