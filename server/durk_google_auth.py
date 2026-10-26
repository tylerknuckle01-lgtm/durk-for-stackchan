"""One-time Google sign-in for Durk. Run from the xiaozhi-server folder with the venv python."""
import os, sys
from google_auth_oauthlib.flow import InstalledAppFlow
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
BASE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.join(BASE, "data", "google_client.json")
TOKEN = os.path.join(BASE, "data", "google_token.json")
SCOPES = ["https://www.googleapis.com/auth/calendar.readonly",
          "https://www.googleapis.com/auth/spreadsheets"]
if not os.path.exists(CLIENT):
    sys.exit(f"Missing {CLIENT}. Download the Desktop OAuth client JSON there first.")
creds = InstalledAppFlow.from_client_secrets_file(CLIENT, SCOPES).run_local_server(
    port=0, prompt="consent", access_type="offline",
    success_message="Durk is connected to Google. You can close this tab.")
with open(TOKEN, "w") as f:
    f.write(creds.to_json())
print("Saved", TOKEN, "refresh token:", bool(creds.refresh_token))
from googleapiclient.discovery import build
cal = build("calendar", "v3", credentials=creds, cache_discovery=False).calendarList().get(calendarId="primary").execute()
print("Calendar OK:", cal.get("id"))
import os
sheet_id = os.environ.get("DURK_JOB_SHEET_ID", "")
if sheet_id:  # optional job tracker
    sh = build("sheets", "v4", credentials=creds, cache_discovery=False).spreadsheets().get(
        spreadsheetId=sheet_id, fields="properties.title").execute()
    print("Sheet OK:", sh["properties"]["title"])
