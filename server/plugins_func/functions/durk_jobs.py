from plugins_func.register import register_function, ToolType, ActionResponse, Action
from config.logger import setup_logging

TAG = __name__
logger = setup_logging()
SPEAK = " Answer in plain spoken English, briefly (2 to 4 sentences), summarizing rather than reading every line."


def _err(e):
    from core import durk_google
    if isinstance(e, durk_google.NotAuthorized):
        return ActionResponse(Action.REQLLM, "Tell the user: my Google connection isn't set up yet, so I can't see that.", None)
    logger.bind(tag=TAG).error(f"Durk Google tool failed: {e}")
    return ActionResponse(Action.REQLLM, "Tell the user you couldn't reach Google right now and to try again shortly.", None)


GET_SCHEDULE_DESC = {"type": "function", "function": {
    "name": "get_schedule",
    "description": "Read the owner's Google Calendar for a day. Use for 'what's on my schedule today', "
                   "'what do I have tomorrow', 'am I free Friday'.",
    "parameters": {"type": "object", "properties": {
        "day": {"type": "string", "description": "'today', 'tomorrow', a weekday name, or YYYY-MM-DD. Default today."}},
        "required": []}}}


@register_function("get_schedule", GET_SCHEDULE_DESC, ToolType.SYSTEM_CTL)
def get_schedule(conn, day: str = "today"):
    from core import durk_google
    d = durk_google.parse_day(day)
    if not d:
        return ActionResponse(Action.REQLLM, f"Couldn't understand the day '{day}'. Ask which day.", None)
    try:
        return ActionResponse(Action.REQLLM, durk_google.schedule_text(d) + SPEAK, None)
    except Exception as e:
        return _err(e)


GET_JOBS_DESC = {"type": "function", "function": {
    "name": "get_jobs",
    "description": "Read the job tracker sheet. Use for 'what jobs do I have', 'what's overdue', "
                   "'what does Reuben have this week'. Only set include_pay if the user explicitly asks about "
                   "pay, piece rate or revenue.",
    "parameters": {"type": "object", "properties": {
        "filter": {"type": "string", "enum": ["open", "due_today", "overdue", "this_week", "delayed", "completed", "all"],
                   "description": "Default 'open' (not completed)."},
        "tech": {"type": "string", "description": "Optional tech name filter, e.g. 'Reuben', 'Luke'."},
        "include_pay": {"type": "boolean", "description": "Include piece rate / tech pay / revenue. Default false."}},
        "required": []}}}


@register_function("get_jobs", GET_JOBS_DESC, ToolType.SYSTEM_CTL)
def get_jobs(conn, filter: str = "open", tech: str = "", include_pay: bool = False):
    from core import durk_google
    if not durk_google.SHEET_ID:
        return ActionResponse(Action.REQLLM, "Tell the user the job tracker is not enabled (set DURK_JOB_SHEET_ID).", None)
    try:
        return ActionResponse(Action.REQLLM, durk_google.jobs_text(filter, tech, bool(include_pay)) + SPEAK, None)
    except Exception as e:
        return _err(e)


ADD_JOB_DESC = {"type": "function", "function": {
    "name": "add_job",
    "description": "Add a new job row to the job tracker. ALWAYS call first with confirmed=false to get a "
                   "read-back, say it to the user, and only call again with confirmed=true after the user clearly says yes.",
    "parameters": {"type": "object", "properties": {
        "community": {"type": "string", "description": "Community / builder, e.g. 'parkvue'."},
        "address": {"type": "string", "description": "Street address, optional short note in parentheses."},
        "job_type": {"type": "string", "description": "Trim, punch, service, warranty, Rough, retro."},
        "tech": {"type": "string", "description": "Assigned tech, e.g. 'Luke / Anag', 'Reuben'. May be empty."},
        "due": {"type": "string", "description": "Due date: 'today', 'tomorrow', weekday, or YYYY-MM-DD."},
        "confirmed": {"type": "boolean", "description": "true only after the user confirmed the read-back."}},
        "required": ["address", "job_type", "due"]}}}


@register_function("add_job", ADD_JOB_DESC, ToolType.SYSTEM_CTL)
def add_job(conn, address: str = "", job_type: str = "", due: str = "", community: str = "", tech: str = "",
            confirmed: bool = False):
    from core import durk_google
    if not durk_google.SHEET_ID:
        return ActionResponse(Action.REQLLM, "Tell the user the job tracker is not enabled (set DURK_JOB_SHEET_ID).", None)
    d = durk_google.parse_day(due)
    if not address or not job_type or not d:
        return ActionResponse(Action.REQLLM, "Some job details are missing or the due date is unclear. Ask for them.", None)
    summary = (f"{job_type} at {community + ' ' if community else ''}{address}, "
               f"{'for ' + tech if tech else 'unassigned'}, due {d.strftime('%A, %B')} {d.day}")
    if not confirmed:
        return ActionResponse(Action.REQLLM, f"Read this back and ask 'Should I add it?': {summary}. "
                                             "Do not add it until the user says yes.", None)
    try:
        durk_google.add_job_row(community.strip().lower(), address.strip().lower(), job_type.strip(), tech.strip(), d)
        return ActionResponse(Action.REQLLM, f"Added to the job tracker: {summary}. Confirm in one short sentence.", None)
    except Exception as e:
        return _err(e)
