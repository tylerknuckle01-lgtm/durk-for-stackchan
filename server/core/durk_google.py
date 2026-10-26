"""Durk: Google Calendar + optional job tracker sheet access (OAuth desktop client, token in data/)."""
import os, re, time, threading
from datetime import datetime, timedelta, date
from zoneinfo import ZoneInfo
from config.logger import setup_logging

TAG = __name__
logger = setup_logging()
TZ = ZoneInfo(os.environ.get("DURK_TZ", "UTC"))
BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIENT_FILE = os.path.join(BASE, "data", "google_client.json")
TOKEN_FILE = os.path.join(BASE, "data", "google_token.json")
SCOPES = ["https://www.googleapis.com/auth/calendar.readonly",
          "https://www.googleapis.com/auth/spreadsheets"]
# Optional job tracker: set DURK_JOB_SHEET_ID to a Google Sheet ID to enable (off by default).
SHEET_ID = os.environ.get("DURK_JOB_SHEET_ID", "")
SHEET_TAB = "Sheet1"
CALENDAR_ID = "primary"
_lock = threading.Lock()
_cache = {}


class NotAuthorized(Exception):
    pass


def _creds():
    if not os.path.exists(TOKEN_FILE):
        raise NotAuthorized("Google isn't connected yet (no token). Run durk_google_auth.py on the PC.")
    from google.oauth2.credentials import Credentials
    from google.auth.transport.requests import Request
    c = Credentials.from_authorized_user_file(TOKEN_FILE, SCOPES)
    if not c.valid:
        if c.expired and c.refresh_token:
            c.refresh(Request())
            with open(TOKEN_FILE, "w") as f:
                f.write(c.to_json())
        else:
            raise NotAuthorized("Google token is invalid. Re-run durk_google_auth.py.")
    return c


def _svc(name, ver):
    from googleapiclient.discovery import build
    return build(name, ver, credentials=_creds(), cache_discovery=False)


def _cached(key, ttl, fn):
    with _lock:
        hit = _cache.get(key)
        if hit and time.time() - hit[0] < ttl:
            return hit[1]
    v = fn()
    with _lock:
        _cache[key] = (time.time(), v)
    return v


def today():
    return datetime.now(TZ).date()


def parse_day(text):
    t = (text or "today").strip().lower()
    d = today()
    if t in ("", "today"): return d
    if t == "tomorrow": return d + timedelta(days=1)
    if t == "yesterday": return d - timedelta(days=1)
    days = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
    for i, n in enumerate(days):
        if n in t:
            return d + timedelta(days=(i - d.weekday()) % 7 or 7)
    for fmt in ("%Y-%m-%d", "%m/%d/%Y", "%m/%d"):
        try:
            x = datetime.strptime(t, fmt).date()
            return x.replace(year=d.year) if fmt == "%m/%d" else x
        except ValueError:
            pass
    return None


def _clock(dt):
    return dt.strftime("%I:%M %p").lstrip("0").replace(":00 ", " ")


# ---------------- calendar ----------------
def get_events(day):
    def fetch():
        start = datetime.combine(day, datetime.min.time(), TZ)
        r = _svc("calendar", "v3").events().list(
            calendarId=CALENDAR_ID, timeMin=start.isoformat(), timeMax=(start + timedelta(days=1)).isoformat(),
            singleEvents=True, orderBy="startTime", maxResults=25).execute()
        out = []
        for e in r.get("items", []):
            if e.get("status") == "cancelled": continue
            s = e.get("start", {})
            if "dateTime" in s:
                when = _clock(datetime.fromisoformat(s["dateTime"]).astimezone(TZ))
            else:
                when = "all day"
            out.append({"when": when, "title": e.get("summary") or "(no title)", "where": e.get("location") or ""})
        return out
    return _cached(("cal", day.isoformat()), 120, fetch)


def schedule_text(day):
    ev = get_events(day)
    if not ev:
        return f"No calendar events on {day.strftime('%A, %B')} {day.day}."
    lines = [f"- {e['when']}: {e['title']}" + (f" at {e['where']}" if e['where'] else "") for e in ev]
    return f"Calendar for {day.strftime('%A, %B')} {day.day} ({len(ev)} events):\n" + "\n".join(lines)


# ---------------- job sheet ----------------
# A community | B address | C job type | D tech | E status | F today | G days until due | H due | I completed
# J invoice service | K piece rate | L tech pay | M WH revenue   (N+ = totals block, ignored)
def _d(s):
    s = (s or "").strip()
    for fmt in ("%m/%d/%Y", "%m/%d/%y", "%Y-%m-%d"):
        try: return datetime.strptime(s, fmt).date()
        except ValueError: pass
    return None


def load_rows():
    def fetch():
        r = _svc("sheets", "v4").spreadsheets().values().get(
            spreadsheetId=SHEET_ID, range=f"{SHEET_TAB}!A1:M1000", valueRenderOption="FORMATTED_VALUE").execute()
        return r.get("values", [])
    return _cached("sheet", 300, fetch)


def get_jobs_list():
    rows = load_rows()
    jobs = []
    for i, r in enumerate(rows[1:], start=2):
        r = (r + [""] * 13)[:13]
        comm, addr, jtype, tech, status = [x.strip() for x in r[:5]]
        if not addr or not jtype:
            continue  # section labels ("week 1", "punch items") and blank formula rows
        if status == "" and tech.lower().startswith("delayed"):
            status, tech = "Delayed", ""
        note = ""
        m = re.match(r"^(.*?)\s*\((.*)\)\s*(.*)$", addr)
        if m:
            addr, note = m.group(1), (m.group(2) + " " + m.group(3)).strip()
        jobs.append({"row": i, "community": comm, "address": addr, "note": note, "type": jtype, "tech": tech,
                     "status": status or "Not started", "due": _d(r[7]), "completed": _d(r[8]),
                     "invoice": r[9].strip(), "piece_rate": r[10].strip(), "tech_pay": r[11].strip(),
                     "revenue": r[12].strip()})
    return jobs


def jobs_text(filter="open", tech="", include_pay=False):
    jobs = get_jobs_list()
    t = today()
    f = (filter or "open").lower()
    done = lambda j: j["status"].lower().startswith("complete")
    if f in ("all",): sel = jobs
    elif f in ("completed", "done"): sel = [j for j in jobs if done(j)]
    elif f in ("due_today", "today"): sel = [j for j in jobs if not done(j) and j["due"] == t]
    elif f == "overdue": sel = [j for j in jobs if not done(j) and j["due"] and j["due"] < t]
    elif f in ("this_week", "week"):
        end = t + timedelta(days=6 - t.weekday())
        sel = [j for j in jobs if not done(j) and (not j["due"] or j["due"] <= end)]
    elif f == "delayed": sel = [j for j in jobs if j["status"].lower().startswith("delay")]
    else: sel = [j for j in jobs if not done(j)]
    if tech:
        sel = [j for j in sel if tech.lower() in j["tech"].lower()]
    sel.sort(key=lambda j: (j["due"] is None, j["due"] or date.max))
    if not sel:
        return f"No jobs match filter '{f}'" + (f" for {tech}" if tech else "") + "."
    lines = []
    for j in sel[:25]:
        due = ("no due date" if not j["due"] else
               "OVERDUE since " + j["due"].strftime("%b %d") if j["due"] < t and j["status"] != "Completed" else
               "due " + ("today" if j["due"] == t else j["due"].strftime("%a %b %d")))
        s = f"- {j['community']} {j['address']} | {j['type']} | {j['tech'] or 'unassigned'} | {j['status']} | {due}"
        if j["note"]: s += f" | note: {j['note']}"
        if include_pay:
            s += f" | piece rate {j['piece_rate'] or '-'}, tech pay {j['tech_pay'] or '-'}, WH revenue {j['revenue'] or '-'}"
        lines.append(s)
    more = f"\n(+{len(sel) - 25} more not listed)" if len(sel) > 25 else ""
    return f"{len(sel)} jobs (filter '{f}'" + (f", tech {tech}" if tech else "") + f", today {t:%m/%d/%Y}):\n" + "\n".join(lines) + more


def add_job_row(community, address, job_type, tech, due):
    rows = load_rows() if False else None
    svc = _svc("sheets", "v4")
    vals = svc.spreadsheets().values().get(spreadsheetId=SHEET_ID, range=f"{SHEET_TAB}!B1:B1000").execute().get("values", [])
    last = max([i + 1 for i, v in enumerate(vals) if v and v[0].strip()] or [1])
    r = last + 1
    data = [{"range": f"{SHEET_TAB}!A{r}:E{r}", "values": [[community, address, job_type, tech, "Not started"]]},
            {"range": f"{SHEET_TAB}!H{r}", "values": [[f"{due.month}/{due.day}/{due.year}"]]}]
    svc.spreadsheets().values().batchUpdate(spreadsheetId=SHEET_ID,
                                            body={"valueInputOption": "USER_ENTERED", "data": data}).execute()
    with _lock:
        _cache.pop("sheet", None)
    logger.bind(tag=TAG).info(f"Durk added job row {r}: {community} {address} {job_type} {tech} {due}")
    return r


# ---------------- rundown ----------------
def rundown_section():
    """Text block for the morning rundown; never raises."""
    parts = []
    t = today()
    try:
        parts.append(schedule_text(t))
    except NotAuthorized:
        return "Calendar and jobs: Google not connected yet (skip this topic)."
    except Exception as e:
        logger.bind(tag=TAG).error(f"rundown calendar failed: {e}")
        parts.append("Calendar unavailable.")
    if not SHEET_ID:
        return "\n".join(parts)
    try:
        jobs = [j for j in get_jobs_list() if not j["status"].lower().startswith("complete")]
        due = [j for j in jobs if j["due"] == t]
        over = [j for j in jobs if j["due"] and j["due"] < t]
        nodate = [j for j in jobs if not j["due"]]
        per = {}
        for j in due + over:
            per[j["tech"] or "unassigned"] = per.get(j["tech"] or "unassigned", 0) + 1
        parts.append(f"Jobs: {len(due)} due today, {len(over)} overdue, {len(nodate)} unscheduled punch items. "
                     f"Due/overdue by tech: " + (", ".join(f"{k} {v}" for k, v in per.items()) or "none") + ".")
        for j in (over + due)[:4]:
            parts.append(f"- {j['community']} {j['address']} ({j['type']}, {j['tech'] or 'unassigned'}, "
                         f"{'overdue' if j in over else 'due today'})")
    except Exception as e:
        logger.bind(tag=TAG).error(f"rundown jobs failed: {e}")
        if SHEET_ID:
            parts.append("Job tracker unavailable.")
    return "\n".join(parts)
