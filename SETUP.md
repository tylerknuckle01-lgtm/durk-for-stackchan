# Durk setup guide (beginner friendly)

Plan on 1–2 hours the first time. You need a Windows PC that stays on (Mac/Linux work too,
commands differ slightly) and the robot on the same home Wi-Fi.

## 1. What you need
- **M5Stack CoreS3 Stack-chan kit** (CoreS3 + servo body with SCS0009 servos and the LED ring).
- A **microSD card** (any size, FAT32) — optional, used for logs, sounds, faces and memory copy.
- A **USB-C data cable**.
- An **xAI API key** from https://console.x.ai (pay as you go; light daily use is usually under a dollar or two a month).

## 2. Install the tools on your PC
1. **Python 3.10–3.12**: https://www.python.org/downloads/ — tick "Add Python to PATH".
2. **Git**: https://git-scm.com/downloads
3. **esptool** (for flashing): `pip install esptool`
4. Only if you build the firmware yourself: **ESP-IDF v6.0.1** (https://docs.espressif.com/projects/esp-idf/).

## 3. Find your PC's IP address
Open Command Prompt, run `ipconfig`, and note the IPv4 address (looks like `192.168.x.x`).
Below, that's `YOUR_PC_IP`. Tip: reserve it in your router so it never changes.

## 4. Install the server
```
git clone https://github.com/xinnan-tech/xiaozhi-esp32-server.git
cd xiaozhi-esp32-server
git checkout <commit in server/UPSTREAM_COMMIT>
cd main\xiaozhi-server
git apply --recount <path to this repo>\server\upstream-changes.patch
```
Then copy everything from this repo's `server\` folder into `main\xiaozhi-server` (overwrite when asked).

```
python -m venv venv
.\venv\Scripts\pip install -r requirements.txt tzdata google-api-python-client google-auth google-auth-oauthlib
```
Download the speech-recognition model the stock server needs (see the xiaozhi-esp32-server README,
"FunASR model").

## 5. Config file and xAI key
```
copy config.template.yaml data\.config.yaml
notepad data\.config.yaml
```
- Replace every `YOUR_XAI_API_KEY` with your key.
- Replace every `YOUR_PC_IP` with your PC's IP.
- Edit the `prompt:` at the top to tell Durk about you (name, interests). Keep it short.

Never share or commit `data\.config.yaml` — it holds your key.

**Time zone:** set it once (use your own zone name from
https://en.wikipedia.org/wiki/List_of_tz_database_time_zones):
```
setx DURK_TZ "America/Chicago"
setx DURK_LOCATION "Chicago, IL"
```
`DURK_LOCATION` is the default city for weather and the morning rundown.

## 6. Start the server
```
powershell -ExecutionPolicy Bypass -File .\start-durk.ps1
```
Check http://YOUR_PC_IP:8003/xiaozhi/ota/ opens in a browser. If the robot can't connect later,
allow TCP ports **8000** and **8003** in Windows Firewall (Private networks).
To start on boot, add `start-durk.ps1` to Task Scheduler ("At log on").

## 7. Flash the robot
Plug the CoreS3 into USB. Find its COM port in Device Manager (e.g. `COM7`).

**Option A — prebuilt image (easiest).** Download `durk-full.bin` from the Releases page, then:
```
python -m esptool --chip esp32s3 -p COM7 -b 921600 erase-flash
python -m esptool --chip esp32s3 -p COM7 -b 921600 write-flash 0x0 durk-full.bin
```
The prebuilt image uses UTC time on the robot's clock display; the server's `DURK_TZ` still makes
reminders and answers use your time zone. Build it yourself (Option B) to change the robot's own clock.

**Option B — build it yourself.**
```
cd firmware
cp sdkconfig.durk sdkconfig
idf.py menuconfig
```
In menuconfig you can set:
- `CONFIG_OTA_URL` = `http://YOUR_PC_IP:8003/xiaozhi/ota/` (optional — you can also type the address on the setup page)
- `CONFIG_DURK_TZ` = your POSIX time zone, e.g. `EST5EDT,M3.2.0,M11.1.0` (Eastern),
  `CST6CDT,M3.2.0,M11.1.0` (Central), `MST7MDT,M3.2.0,M11.1.0` (Mountain), `PST8PDT,M3.2.0,M11.1.0` (Pacific)

Then `idf.py build` and flash with the "Full" command in [README.md](README.md#flash).
After the first full flash you can update with the faster "App only" command.

## 8. Connect the robot to Wi-Fi and your server
After flashing, the robot opens a hotspot named `Xiaozhi-XXXX`.
1. Join it from your phone. The setup page opens (if not, browse to `http://192.168.4.1`).
2. Pick your home Wi-Fi (2.4 GHz) and enter the password.
3. In **Server address**, type your PC's IP from step 3, e.g. `192.168.1.20`.
   You can also paste the full `ws://192.168.1.20:8000/xiaozhi/v1/` from your config — the robot
   works out the rest (it talks to your PC on port 8003 first, which tells it the 8000 address).
4. Tap Connect. The robot restarts, connects to your PC, and says hello.

To change the server later, get back to the setup page (the robot reopens the hotspot when it can't join
Wi-Fi; otherwise run the `erase-flash` + flash steps again) and type a new address.
Leaving the field empty keeps the address the firmware was built with.

## 9. Optional: Google Calendar
1. In https://console.cloud.google.com create a project, enable the **Google Calendar API**
   (and **Google Sheets API** if you want the job tracker).
2. OAuth consent screen → External, add yourself as a test user.
3. Credentials → Create OAuth client → **Desktop app** → download JSON → save as `data\google_client.json`.
4. Run `.\venv\Scripts\python.exe durk_google_auth.py` and sign in. It writes `data\google_token.json`.
5. Restart the server. Ask "What's on my schedule today?"

Optional job tracker: set `setx DURK_JOB_SHEET_ID "<your sheet id>"` (columns: community, address,
job type, tech, status, due…). Leave it unset to keep job tools off.

## 10. Try it
"Hey Durk, what time is it?" · "Remind me in 10 minutes to stretch" · "Look left" ·
"Set a 5 minute timer" · "Galaxy mode" · "Remember that my dog is named Max".

## Troubleshooting
| Problem | Fix |
|---|---|
| Robot says it can't connect / shows server error | Server running? Same Wi-Fi? Right IP in the setup page's Server address and in `data\.config.yaml`? Firewall ports 8000/8003 open? |
| Boot loops after flashing | Do a full erase + full flash. Make sure you used the 16 MB settings in the README. |
| `esptool` can't find the port | Use a data USB cable; hold the reset button while plugging in; check Device Manager. |
| Durk hears you but never answers | Check `server.err` for an xAI error (wrong key or no credit). |
| Wrong time | Set `DURK_TZ` (server) and `CONFIG_DURK_TZ` (firmware). |
| Calendar says "not set up" | Redo step 9; delete `data\google_token.json` and re-run the auth script. |
| Head doesn't move | Check servo cable; say "turn on look-around"; reboot the robot. |
