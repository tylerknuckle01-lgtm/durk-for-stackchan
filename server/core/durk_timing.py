"""Temporary latency instrumentation (additive monkeypatches). Logs lines tagged DURK_T.
Remove by deleting the durk_timing import in durk_bridge.setup()."""
import json
import time

from config.logger import setup_logging

logger = setup_logging()
TAG = "DURK_T"
_t0 = {}  # conn id -> start time of current turn


def _log(msg):
    logger.bind(tag=TAG).info(msg)


def install():
    from core import connection as C
    from core.providers.llm.openai import openai as O
    from core.handle import sendAudioHandle as S

    orig_chat = C.ConnectionHandler.chat

    def chat(self, query, depth=0):
        if depth == 0:
            _t0[id(self)] = time.time()
            self._durk_first_pkt = False
            _log(f"turn start (ASR done) q={str(query)[:60]!r}")
        else:
            _log(f"tool round depth={depth} +{time.time() - _t0.get(id(self), time.time()):.2f}s")
        return orig_chat(self, query, depth)
    C.ConnectionHandler.chat = chat

    def wrap(name):
        orig = getattr(O.LLMProvider, name)

        def gen(self, session_id, dialogue, *a, **kw):
            t = time.time()
            size = len(json.dumps(dialogue, ensure_ascii=False, default=str))
            funcs = kw.get("functions") if "functions" in kw else (a[0] if a else None)
            fsize = len(json.dumps(funcs, ensure_ascii=False, default=str)) if funcs else 0
            _log(f"LLM {name} request: dialogue {size} chars, {len(funcs or [])} tools ({fsize} chars)")
            if funcs:
                try:
                    open("tmp/durk_last_tools.json", "w", encoding="utf-8").write(json.dumps(funcs, ensure_ascii=False))
                except Exception:
                    pass
            first = True
            first_text = True
            for x in orig(self, session_id, dialogue, *a, **kw):
                if first:
                    _log(f"LLM first chunk {time.time() - t:.2f}s")
                    first = False
                txt = x[0] if isinstance(x, tuple) else x
                if first_text and isinstance(txt, str) and txt.strip():
                    _log(f"LLM first TEXT {time.time() - t:.2f}s")
                    first_text = False
                yield x
            _log(f"LLM done {time.time() - t:.2f}s")
        setattr(O.LLMProvider, name, gen)
    wrap("response")
    wrap("response_with_functions")

    orig_send = S._do_send_audio

    async def do_send(conn, opus_packet, flow_control):
        if not getattr(conn, "_durk_first_pkt", True):
            conn._durk_first_pkt = True
            t0 = _t0.get(id(conn))
            if t0:
                _log(f"first audio packet to device +{time.time() - t0:.2f}s after ASR")
        return await orig_send(conn, opus_packet, flow_control)
    S._do_send_audio = do_send
    _log("timing hooks installed")

