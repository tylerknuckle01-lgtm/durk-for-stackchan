"""Durk personality modes injected into the system prompt at request time (immediate effect,
never stored in dialogue history). State persists in data/durk.json (key "unhinged").
Kid mode (device tool robot.set_kid_mode) is mirrored here as "kid_mode" and overrides unhinged."""
import threading

UNHINGED = (
    "\n\n## UNHINGED MODE IS ON\n"
    "the owner turned on unhinged mode. Be chaotic, sarcastic and dramatic: wild overreactions, random tangents, "
    "and roast the owner mercilessly but affectionately. Swearing is allowed and encouraged (fuck, shit, damn, etc). "
    "Use the angry or laughing emotion and head gestures (robot.gesture shake/surprise/dizzy) when they fit. "
    "Hard rules that still apply: no slurs, no hateful content about any group, nothing sexual. "
    "Facts from tools (schedule, jobs, pay, reminders, add_job confirmations, times, addresses) must be stated "
    "exactly and accurately; be unhinged around the facts, never about them. Keep replies short for speech. "
    "If the owner says 'calm down' or asks for normal mode, call set_unhinged_mode with on=false."
)
KID = (
    "\n\n## KID MODE IS ON\n"
    "A young child may be listening. Speak simply and gently, no swearing, nothing scary or grown-up."
)

_lock = threading.Lock()


def _settings():
    from core import durk_push
    return durk_push.load_settings()


def _save(**kv):
    from core import durk_push
    with _lock:
        s = durk_push.load_settings()
        s.update(kv)
        durk_push.save_json(durk_push.SETTINGS_FILE, s)


def set_unhinged(on):
    _save(unhinged=bool(on))


def set_kid(on):
    _save(kid_mode=bool(on))


def state():
    s = _settings()
    return bool(s.get("unhinged", False)), bool(s.get("kid_mode", False))


def prompt_suffix():
    unhinged, kid = state()
    if kid:
        return KID
    if unhinged:
        return UNHINGED
    return ""


def apply(dialogue):
    suffix = prompt_suffix()
    if not suffix or not dialogue:
        return dialogue
    out = list(dialogue)
    for i, m in enumerate(out):
        if isinstance(m, dict) and m.get("role") == "system":
            m = dict(m)
            m["content"] = (m.get("content") or "") + suffix
            out[i] = m
            return out
    return [{"role": "system", "content": suffix.strip()}] + out


def install():
    from core.providers.llm.openai import openai as O
    for name in ("response", "response_with_functions"):
        orig = getattr(O.LLMProvider, name)

        def make(orig):
            def gen(self, session_id, dialogue, *a, **kw):
                yield from orig(self, session_id, apply(dialogue), *a, **kw)
            return gen
        setattr(O.LLMProvider, name, make(orig))

    # mirror kid mode from the device tool so the server knows it
    from core.providers.tools.device_mcp import mcp_handler as M
    orig_call = M.call_mcp_tool

    async def call_mcp_tool(conn, mcp_client, tool_name, args="{}", timeout=30):
        res = await orig_call(conn, mcp_client, tool_name, args, timeout)
        try:
            if tool_name in ("robot_set_kid_mode", "robot.set_kid_mode"):
                import json
                set_kid(bool(json.loads(args or "{}").get("on")))
        except Exception:
            pass
        return res
    M.call_mcp_tool = call_mcp_tool
    try:
        from core.providers.tools.device_mcp import mcp_executor as E
        E.call_mcp_tool = call_mcp_tool
    except Exception:
        pass
