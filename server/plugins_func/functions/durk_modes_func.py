from plugins_func.register import register_function, ToolType, ActionResponse, Action

SET_UNHINGED_DESC = {
    "type": "function",
    "function": {
        "name": "set_unhinged_mode",
        "description": (
            "Turn Durk's unhinged mode on or off. ON when the owner says things like 'go unhinged', 'unhinged mode', "
            "'get wild'. OFF when he says 'calm down', 'normal mode', 'be nice'."
        ),
        "parameters": {"type": "object", "properties": {"on": {"type": "boolean"}}, "required": ["on"]},
    },
}


@register_function("set_unhinged_mode", SET_UNHINGED_DESC, ToolType.SYSTEM_CTL)
def set_unhinged_mode(conn, on: bool = False):
    from core import durk_modes
    durk_modes.set_unhinged(on)
    _, kid = durk_modes.state()
    if on and kid:
        msg = "Unhinged mode is saved as ON, but kid mode is on so you must stay clean. Tell the owner briefly."
    elif on:
        msg = "Unhinged mode is ON now. Announce it in one short, chaotic, sweary line."
    else:
        msg = "Unhinged mode is OFF. Confirm calmly in one short sentence."
    return ActionResponse(Action.REQLLM, msg, None)
