import sys, time, types
import os; sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from core import durk_slow as S
said = []
class Conn: pass
def stream(delays, chunks):
    def f():
        for d, c in zip(delays, chunks):
            time.sleep(d); yield c
    return f
def run(label, makers, expect_filler, expect_text):
    c = Conn(); c._durk_filler_done = False; c._durk_text_started = False; said.clear()
    it = iter(makers)
    t = time.time()
    out = list(S.guarded(lambda: next(it)(), c, speak=lambda conn: said.append("filler") or "filler", filler_after=0.3, retry_after=0.6))
    txt = "".join(x[0] or "" for x in out)
    ok = (bool(said) == expect_filler) and txt == expect_text and len(said) <= 1
    print(f"{'PASS' if ok else 'FAIL'} {label}: filler={said} text={txt!r} {time.time()-t:.2f}s")
run("fast reply", [stream([0.05, 0.05], [("Hi ", None), ("there.", None)])], False, "Hi there.")
run("slow text, chunks early", [stream([0.1, 0.5], [("", None), ("Late.", None)])], True, "Late.")
run("text before filler time", [stream([0.1, 0.5], [("Early ", None), ("rest.", None)])], False, "Early rest.")
run("stalled -> retry ok", [stream([5], [("never", None)]), stream([0.05], [("Retry.", None)])], True, "Retry.")
run("tool-call only", [stream([0.1, 0.4], [(None, [{"id": "1"}]), (None, [{"id": "1"}])])], True, "")
