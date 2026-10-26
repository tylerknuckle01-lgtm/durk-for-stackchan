"""Slow-reply handling (additive monkeypatch on the OpenAI-compatible LLM provider).
  * filler: no spoken text ~3 s after the request -> Durk says a short filler line (once per turn,
    through the normal TTS queue, never added to the dialogue history).
  * retry: no first chunk within ~6 s -> abandon that stream and retry the same request once.
Both log under DURK_T."""
import queue
import random
import threading
import time

FILLER_AFTER_S = 3.0
RETRY_AFTER_S = 6.0
FILLERS = ["Hmm, one sec.", "Hmm, let me think.", "One sec.", "Okay, hang on.", "Hmm, give me a second."]

_tl = threading.local()


def _log(msg):
    try:
        from config.logger import setup_logging
        setup_logging().bind(tag="DURK_T").info(msg)
    except Exception:
        print("DURK_T", msg)


def _is_text(x):
    c = x[0] if isinstance(x, tuple) else (x.get("content") if isinstance(x, dict) else x)
    return isinstance(c, str) and c.strip() != ""


def _speak_filler(conn):
    from core.providers.tts.dto.dto import TTSMessageDTO, SentenceType, ContentType
    text = random.choice(FILLERS)
    conn.tts.tts_text_queue.put(TTSMessageDTO(sentence_id=conn.sentence_id, sentence_type=SentenceType.MIDDLE,
                                              content_type=ContentType.TEXT, content_detail=text))
    return text


def guarded(make_stream, conn=None, speak=None, filler_after=FILLER_AFTER_S, retry_after=RETRY_AFTER_S):
    """Generator yielding the chunks of make_stream() with filler + one retry. Testable without the server."""
    speak = speak or _speak_filler
    t0 = time.time()
    for attempt in (1, 2):
        q, stop = queue.Queue(), threading.Event()

        def worker():
            g = None
            try:
                g = make_stream()
                for x in g:
                    if stop.is_set():
                        break
                    q.put(("x", x))
                q.put(("end", None))
            except Exception as e:  # noqa
                q.put(("err", e))
            finally:
                try:
                    if g is not None:
                        g.close()
                except Exception:
                    pass
        threading.Thread(target=worker, daemon=True, name=f"durk_llm_{attempt}").start()
        ta = time.time()
        got_first = False
        try:
            while True:
                try:
                    kind, x = q.get(timeout=0.2)
                except queue.Empty:
                    now = time.time()
                    if (conn is not None and not getattr(conn, "_durk_text_started", False)
                            and not getattr(conn, "_durk_filler_done", False) and now - t0 >= filler_after):
                        conn._durk_filler_done = True
                        try:
                            said = speak(conn)
                            _log(f"filler spoken after {now - t0:.2f}s: {said!r}")
                        except Exception as e:
                            _log(f"filler failed: {e}")
                    if attempt == 1 and not got_first and now - ta >= retry_after:
                        _log(f"no first chunk after {now - ta:.2f}s -> retrying LLM request once")
                        break
                    continue
                if kind == "x":
                    got_first = True
                    if conn is not None and _is_text(x):
                        conn._durk_text_started = True
                    yield x
                elif kind == "end":
                    if attempt == 2:
                        _log(f"retry succeeded, finished {time.time() - t0:.2f}s after first request")
                    return
                else:
                    raise x
        finally:
            stop.set()
    # unreachable


def install():
    from core import connection as C
    from core.providers.llm.openai import openai as O

    orig_chat = C.ConnectionHandler.chat

    def chat(self, query, depth=0):
        if depth == 0:
            self._durk_filler_done = False
            self._durk_text_started = False
        prev = getattr(_tl, "conn", None)
        _tl.conn = self
        try:
            return orig_chat(self, query, depth)
        finally:
            _tl.conn = prev
    C.ConnectionHandler.chat = chat

    for name in ("response", "response_with_functions"):
        orig = getattr(O.LLMProvider, name)

        def make(orig):
            def gen(self, session_id, dialogue, *a, **kw):
                conn = getattr(_tl, "conn", None)
                yield from guarded(lambda: orig(self, session_id, dialogue, *a, **kw), conn)
            return gen
        setattr(O.LLMProvider, name, make(orig))
    _log("slow-reply guard installed (filler 3s, retry 6s)")
