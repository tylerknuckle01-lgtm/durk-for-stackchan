# Durk bridge: MCP, smart-home triggers, pet/kid mode

## Server (additive)
* `core/durk_bridge.py` — new routes on :8003
  * `GET /durk/tools`, `POST /durk/tool {name,args}` — call any firmware tool (wakes Durk silently if idle, ~10-20 s first call)
  * `POST /durk/trigger {event, text?, gesture?, llm?}` — speak + gesture. Built-in events: doorbell, motion, garage_open,
    washer_done, package, leaving, arriving, goodnight (any other event name works with `text`)
  * `GET /durk/bridge_status`
* Hook added to `core/http_server.py` right after `durk_push.setup(app, self.config)` (backup: `http_server.py.pre-bridge.bak`):
```python
try:
    from core import durk_bridge
    durk_bridge.setup(app, self.config)
except Exception as _e:
    self.logger.bind(tag=TAG).error(f"durk_bridge disabled: {_e}")
```

## MCP client config (tools/durk_mcp.py, copy at C:\path\to\durk-mcp\durk_mcp.py, no pip installs)
Claude Desktop (`%APPDATA%\Claude\claude_desktop_config.json`):
```json
{ "mcpServers": { "durk": { "command": "python", "args": ["C:\\path\\to\\durk-mcp\\durk_mcp.py"] } } }
```
Claude Code: `claude mcp add durk -- python C:\path\to\durk-mcp\durk_mcp.py`
Tools: say, look, set_head_angles, gesture, see (camera + question), set_led_color, trigger, list_device_tools, call_device_tool.

## Home Assistant (optional)
```yaml
rest_command:
  durk_trigger:
    url: "http://YOUR_PC_IP:8003/durk/trigger"
    method: POST
    content_type: "application/json"
    payload: '{"event": "{{ event }}", "text": "{{ text | default('''') }}"}'
# automation action:
#   - service: rest_command.durk_trigger
#     data: { event: doorbell }
```
curl: `curl -X POST http://YOUR_PC_IP:8003/durk/trigger -H "Content-Type: application/json" -d '{"event":"doorbell"}'`

## Firmware
* Pet mode (`durk_pet.cc`): stroke the screen (slide >70 px) or hold 1 s -> happy face + happy wiggle (+ `/sfx/pet.ogg` if present).
  Shake -> shocked + surprise (`shake.ogg`), spin -> confused + dizzy (`dizzy.ogg`). Only reacts when idle; ignores its own head motion.
  IMU = BMI270 @0x69, init delayed 8 s after boot (early init broke AW88298 init). Short tap still starts/stops a chat.
* Smooth gestures: eased (smoothstep) servo moves for mood gestures; `robot.gesture` tool.
* Kid mode (from dotty-stackchan idea): `robot.set_kid_mode` / `robot.get_modes`; gentler reactions, LLM told to speak simply.
* Tools: robot.set_pet_mode, robot.set_kid_mode, robot.get_modes, robot.gesture.
