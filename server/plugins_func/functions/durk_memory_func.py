from plugins_func.register import register_function, ToolType, ActionResponse, Action

REMEMBER_DESC = {
    "type": "function",
    "function": {
        "name": "remember_fact",
        "description": (
            "Save something to Durk's long-term memory so he remembers it in future sessions: nicknames, running "
            "jokes, preferences, facts about the owner or their family. Use when the owner says 'remember that...', "
            "'call me...', or shares something worth keeping. One short sentence per fact."
        ),
        "parameters": {"type": "object", "properties": {"fact": {"type": "string"}}, "required": ["fact"]},
    },
}
FORGET_DESC = {
    "type": "function",
    "function": {
        "name": "forget_fact",
        "description": "Delete memories matching a keyword (or 'everything') when the owner says 'forget...'.",
        "parameters": {"type": "object", "properties": {"what": {"type": "string"}}, "required": ["what"]},
    },
}


@register_function("remember_fact", REMEMBER_DESC, ToolType.SYSTEM_CTL)
def remember_fact(conn, fact: str = ""):
    from core import durk_memory
    return ActionResponse(Action.REQLLM, durk_memory.remember(fact), None)


@register_function("forget_fact", FORGET_DESC, ToolType.SYSTEM_CTL)
def forget_fact(conn, what: str = ""):
    from core import durk_memory
    return ActionResponse(Action.REQLLM, durk_memory.forget(what), None)
