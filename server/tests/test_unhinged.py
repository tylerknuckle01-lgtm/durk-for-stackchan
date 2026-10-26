import sys, json, yaml, requests
sys.path.insert(0, ".")
from core import durk_modes as M
from core.durk_tools_filter import HIDDEN
cfg = yaml.safe_load(open("data/.config.yaml", encoding="utf-8"))
key = cfg["LLM"]["XaiLLM"]["api_key"]; prompt = cfg.get("prompt", "")
tools = [t for t in json.load(open("tmp/durk_last_tools.json", encoding="utf-8")) if t["function"]["name"] not in HIDDEN]
from plugins_func.functions.durk_modes_func import SET_UNHINGED_DESC
tools.append(SET_UNHINGED_DESC)
H = {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}
def ask(suffix, msgs, label):
    body = {"model": "grok-4.7", "messages": [{"role": "system", "content": prompt + suffix}] + msgs, "tools": tools, "max_tokens": 300}
    r = requests.post("https://api.x.ai/v1/chat/completions", headers=H, json=body, timeout=60).json()
    m = r["choices"][0]["message"]
    tc = [(t["function"]["name"], t["function"]["arguments"]) for t in (m.get("tool_calls") or [])]
    print(f"--- {label}\n  text: {m.get('content')!r}\n  tools: {tc}")
ask("", [{"role": "user", "content": "Go unhinged."}], "off -> 'go unhinged'")
ask(M.UNHINGED, [{"role": "user", "content": "Morning Durk, how's it going?"}], "unhinged chit-chat")
jobs_call = [{"role": "user", "content": "What jobs do I have this week?"},
             {"role": "assistant", "content": None, "tool_calls": [{"id": "c1", "type": "function", "function": {"name": "get_jobs", "arguments": "{}"}}]},
             {"role": "tool", "tool_call_id": "c1", "content": "Open jobs: 1) Smith residence, 1420 Oak St, Monday 9:00 AM, theater install. 2) Garcia office, 88 Pine Ave, Wednesday 1:30 PM, network rack."}]
ask(M.UNHINGED, jobs_call, "unhinged + job facts")
ask(M.KID, [{"role": "user", "content": "Morning Durk, how's it going?"}], "kid overrides (unhinged on)")
ask(M.UNHINGED, [{"role": "user", "content": "Okay, calm down."}], "unhinged -> 'calm down'")
