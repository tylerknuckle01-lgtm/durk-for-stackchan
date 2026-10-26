from plugins_func.register import register_function, ToolType, ActionResponse, Action
from config.logger import setup_logging

TAG = __name__
logger = setup_logging()

SET_REMINDER_FUNCTION_DESC = {
    "type": "function",
    "function": {
        "name": "set_reminder",
        "description": (
            "Set a spoken reminder. At the given time the robot will say the message out loud, "
            "even if it is asleep. Use when the user says things like 'remind me at 3 to call Jessica' "
            "or 'remind me in 20 minutes to check the oven'. Times are the user's local time."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "time": {
                    "type": "string",
                    "description": "When to remind. Prefer 24-hour local 'HH:MM' (e.g. '15:00'), a full local "
                                   "'YYYY-MM-DDTHH:MM' for other days, or 'in N minutes' / 'in N hours'.",
                },
                "message": {
                    "type": "string",
                    "description": "What to remind the user about, phrased to be spoken, e.g. 'call Jessica'.",
                },
            },
            "required": ["time", "message"],
        },
    },
}


@register_function("set_reminder", SET_REMINDER_FUNCTION_DESC, ToolType.SYSTEM_CTL)
def set_reminder(conn, time: str = "", message: str = ""):
    from core import durk_push
    when = durk_push.add_reminder(time, message) if message else None
    if not when:
        return ActionResponse(Action.REQLLM, f"Could not understand the reminder time '{time}'. Ask the user to repeat it.", None)
    return ActionResponse(
        Action.REQLLM,
        f"Reminder saved for {durk_push.speakable_time(when)}: {message}. Confirm it to the user in one short sentence.",
        None,
    )
