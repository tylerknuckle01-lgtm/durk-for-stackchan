"""Hide low-value tools from the LLM to shrink the prompt (latency). Tools stay callable
via /durk/tool and the MCP bridge; only the list sent to Grok is filtered."""
HIDDEN = {
    "get_lunar", "play_music", "change_role", "get_news_from_newsnow",
    "robot_get_logs", "robot_list_sounds", "robot_face_tracking_invert",
    "robot_set_led_brightness", "robot_get_modes",
    "robot_memory_read", "robot_memory_write", "robot_memory_info",
}


def _name(f):
    try:
        return f.get("function", {}).get("name") or f.get("name")
    except Exception:
        return None


def install():
    from core.providers.tools import unified_tool_handler as U
    orig = U.UnifiedToolHandler.get_functions

    def get_functions(self):
        return [f for f in orig(self) if _name(f) not in HIDDEN]
    U.UnifiedToolHandler.get_functions = get_functions
