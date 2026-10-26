from plugins_func.register import register_function, ToolType, ActionResponse, Action

ASSIGN_DESC = {
    "type": "function",
    "function": {
        "name": "assign_nfc_card",
        "description": (
            "Assign an action to the NFC card the owner most recently tapped on Durk's body (after Durk asked "
            "'what should this card do?'). action: say (value = what to say), rundown (morning rundown), "
            "unhinged (value on/off), theme (value = face theme e.g. galaxy, classic), gesture (value = e.g. happy, "
            "nod, shake, dizzy), trigger (value = event e.g. doorbell, goodnight). label: short card name."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["say", "rundown", "unhinged", "theme", "gesture", "trigger"]},
                "value": {"type": "string"},
                "label": {"type": "string"},
            },
            "required": ["action"],
        },
    },
}


@register_function("assign_nfc_card", ASSIGN_DESC, ToolType.SYSTEM_CTL)
def assign_nfc_card(conn, action: str = "", value: str = "", label: str = ""):
    from core import durk_nfc
    return ActionResponse(Action.REQLLM, durk_nfc.assign(action, value, label), None)
