#!/usr/bin/env python3
"""
ClockHub mock web server.

Runs the same web UI (HTML/CSS/JS + form logic) as src/web_server.cpp,
backed by an in-memory copy of `sysConfig` instead of the Arduino/EEPROM.
Useful to test the dashboard from a phone/laptop on the same LAN without
flashing the board.

Usage:
    python tools/mock_server.py [port]

Default port is 8080. The server binds to 0.0.0.0, so any device on the
same network can open http://<this-pc-ip>:<port>/?pin=<ACCESS_PIN>

It reads ACCESS_PIN from the project's .env file (same one used by the
firmware build). If .env is missing, the PIN defaults to "1234".

This is a dev-only approximation:
  - No real WiFi/EEPROM/motor/pump - all state lives in memory and resets
    when the script restarts.
  - The "Dev Tools" card (not present on the real device) lets you jump
    the simulated clock around to test alarm-window locking logic.
"""

import http.server
import socketserver
import socket
import sys
import os
import time
import threading
from datetime import datetime, timedelta
from urllib.parse import urlparse, parse_qs

# --------------------------------------------------------------------------
# Load ACCESS_PIN from .env (same file the firmware build reads)
# --------------------------------------------------------------------------

def load_access_pin():
    env_path = os.path.join(os.path.dirname(__file__), "..", ".env")
    if os.path.isfile(env_path):
        with open(env_path, "r") as f:
            for line in f:
                line = line.strip()
                if line.startswith("ACCESS_PIN="):
                    val = line.split("=", 1)[1].strip()
                    if len(val) >= 2 and val[0] in ('"', "'") and val[0] == val[-1]:
                        val = val[1:-1]
                    return val
    return "1234"


ACCESS_PIN = load_access_pin()

DAYS_OF_WEEK = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]

PIN_MAX_ATTEMPTS = 5
PIN_LOCKOUT_SEC = 300

# --------------------------------------------------------------------------
# In-memory state (mirrors sysConfig + other globals from globals.h)
# --------------------------------------------------------------------------

state_lock = threading.Lock()

state = {
    # Config
    "globalEnabled": True,
    "pumpEnabled": True,
    "runDuration": 10,
    "lightEnabled": True,
    "lightLeadMinutes": 30,
    "fallingAsleepMinutes": 15,
    "schedule": [
        {"active": True, "hour": (10 if i in (0, 6) else 8), "minute": (0 if i in (0, 6) else 30)}
        for i in range(7)
    ],
    "blindEnabled": True,
    "blindLeadMinutes": 30,
    "blindOpenDuration": 150,
    "blindCloseDuration": 150,
    "motorSlowdown": [80, 75, 70, 60, 40, 25],

    # Runtime state
    "manualOverride": False,
    "pumpOnUntil": 0.0,
    "alarmTriggered": False,
    "lightTriggered": False,
    "blindTriggered": False,
    "scheduleErrorMsg": "",
    "pendingAnnounceMsg": "",

    # Blind motor state machine (seconds instead of millis())
    "blindManualActive": False,
    "blindManualDirection": 0,
    "blindRunStartTime": 0.0,
    "blindRunTotalSec": 0.0,
    "blindRunFullSec": 0.0,
    "blindRunStartPos": 0,
    "blindPositionPct": -1,

    # Sleep calculator
    "targetWakeH": 8,
    "targetWakeM": 30,
    "showBedTimes": False,

    # Dev clock offset (seconds added to real time)
    "timeOffsetSec": 0,

    # Firmware / OTA (mirrors ota_manager.cpp + globals.h)
    "firmwareVersion": "dev",
    "otaState": "idle",  # "idle" | "error"
    "otaUpdateAvailable": False,
    "otaLatestVersion": "",
    "otaErrorMsg": "",

    # Dev-only OTA simulation toggles
    "otaSimulateAvailable": False,
    "otaSimulateFailure": False,
}

pin_fail_count = 0
pin_first_fail_time = 0.0

# --------------------------------------------------------------------------
# Simulated time helpers
# --------------------------------------------------------------------------

def now_dt():
    return datetime.now() + timedelta(seconds=state["timeOffsetSec"])


def current_day():
    return now_dt().isoweekday() % 7  # Sunday=0 .. Saturday=6


def current_total_minutes():
    d = now_dt()
    return d.hour * 60 + d.minute


def formatted_time():
    return now_dt().strftime("%H:%M:%S")


def lead_time(alm_total, lead_minutes, cur_day):
    t = alm_total - lead_minutes
    if t < 0:
        d = 6 if cur_day == 0 else cur_day - 1
        t += 1440
    else:
        d = cur_day
    return t, d


def get_wake_time(hours_from_now):
    total_min_to_add = int(hours_from_now * 60) + state["fallingAsleepMinutes"]
    cur_total = current_total_minutes()
    result_min = (cur_total + total_min_to_add) % 1440
    return "%02d:%02d" % (result_min // 60, result_min % 60)


def get_bed_time(h, m, sleep_hours):
    total_min_to_sub = int(sleep_hours * 60) + state["fallingAsleepMinutes"]
    target_total = h * 60 + m
    result_min = target_total - total_min_to_sub
    while result_min < 0:
        result_min += 1440
    return "%02d:%02d" % (result_min // 60, result_min % 60)


# --------------------------------------------------------------------------
# Scheduler logic (ports of scheduler.cpp)
# --------------------------------------------------------------------------

def is_schedule_locked():
    if not state["globalEnabled"]:
        return False

    cur_day = current_day()
    cur_total = current_total_minutes()

    for check_day in range(7):
        if not state["schedule"][check_day]["active"]:
            continue

        alm_total = state["schedule"][check_day]["hour"] * 60 + state["schedule"][check_day]["minute"]

        if check_day == cur_day:
            min_until = alm_total - cur_total
            if min_until < 0:
                min_until += 1440
        elif check_day == (cur_day + 1) % 7:
            min_until = (1440 - cur_total) + alm_total
        else:
            continue

        if min_until <= 60:
            state["scheduleErrorMsg"] = "Some settings cannot be modified because an alarm is within 1 hour"
            return True

    return False


def is_blind_closing_locked():
    if not state["globalEnabled"]:
        return False

    cur_day = current_day()
    cur_total = current_total_minutes()

    for check_day in range(7):
        if not state["schedule"][check_day]["active"]:
            continue

        alm_total = state["schedule"][check_day]["hour"] * 60 + state["schedule"][check_day]["minute"]

        if check_day == cur_day:
            min_until = alm_total - cur_total
            if min_until < 0:
                min_until += 1440
            if min_until <= 30 or min_until >= 1410:
                return True
        elif check_day == (cur_day + 1) % 7:
            min_until = (1440 - cur_total) + alm_total
            if min_until <= 30:
                return True

    return False


def current_blind_position():
    if not state["blindManualActive"] or state["blindRunTotalSec"] == 0 or state["blindRunStartPos"] < 0:
        return state["blindPositionPct"]
    elapsed = time.time() - state["blindRunStartTime"]
    if elapsed >= state["blindRunTotalSec"]:
        return 100 if state["blindManualDirection"] == 1 else 0
    if state["blindManualDirection"] == 1:
        pos = state["blindRunStartPos"] + (100 - state["blindRunStartPos"]) * elapsed / state["blindRunTotalSec"]
    else:
        pos = state["blindRunStartPos"] - state["blindRunStartPos"] * elapsed / state["blindRunTotalSec"]
    return max(0, min(100, int(pos)))


def tick():
    """Non-blocking port of runAlarmLogic(), called before every request."""
    with state_lock:
        # Blind motor state machine
        if state["blindManualActive"]:
            elapsed = time.time() - state["blindRunStartTime"]
            if elapsed >= state["blindRunTotalSec"]:
                state["blindPositionPct"] = 100 if state["blindManualDirection"] == 1 else 0
                state["blindManualActive"] = False
                state["blindManualDirection"] = 0

        if state["manualOverride"]:
            return

        cur_day = current_day()
        cur_total = current_total_minutes()
        sched = state["schedule"][cur_day]
        alm_total = sched["hour"] * 60 + sched["minute"]

        lgt_total, lgt_day = lead_time(alm_total, state["lightLeadMinutes"], cur_day)
        bld_total, bld_day = lead_time(alm_total, state["blindLeadMinutes"], cur_day)

        if state["globalEnabled"]:
            if (state["lightEnabled"] and state["schedule"][lgt_day]["active"]
                    and cur_total == lgt_total and not state["lightTriggered"]):
                state["lightTriggered"] = True

            if (state["blindEnabled"] and not state["blindManualActive"]
                    and state["schedule"][bld_day]["active"] and cur_total == bld_total
                    and not state["blindTriggered"]):
                start_pos = 0 if state["blindPositionPct"] == -1 else state["blindPositionPct"]
                remain = 100 - start_pos
                if remain > 0:
                    state["blindRunStartPos"] = start_pos
                    state["blindManualActive"] = True
                    state["blindManualDirection"] = 1
                    state["blindRunStartTime"] = time.time()
                    state["blindRunFullSec"] = float(state["blindOpenDuration"])
                    state["blindRunTotalSec"] = state["blindRunFullSec"] * remain / 100.0
                state["blindTriggered"] = True

            if state["pumpEnabled"] and sched["active"] and cur_total == alm_total:
                if not state["alarmTriggered"]:
                    state["pumpOnUntil"] = time.time() + state["runDuration"]
                    state["alarmTriggered"] = True
            else:
                state["alarmTriggered"] = False
                if cur_total != lgt_total:
                    state["lightTriggered"] = False
                if cur_total != bld_total:
                    state["blindTriggered"] = False
        else:
            state["pumpOnUntil"] = 0.0
            state["alarmTriggered"] = False
            state["lightTriggered"] = False
            state["blindTriggered"] = False


# --------------------------------------------------------------------------
# Route handlers (port of handleRoutes() in web_server.cpp)
# --------------------------------------------------------------------------

def qflag(qs, key):
    """True if a checkbox named `key` was submitted (i.e. checked)."""
    return key in qs


def qint(qs, key, default=None):
    if key in qs and qs[key][0] != "":
        try:
            return int(qs[key][0])
        except ValueError:
            return default
    return default


def handle_routes(path, qs):
    with state_lock:
        if path == "/TOGGLE":
            state["manualOverride"] = not state["manualOverride"]
            if not state["manualOverride"]:
                state["pumpOnUntil"] = 0.0

        elif path == "/SAVE_ALL":
            if not is_schedule_locked():
                dur = qint(qs, "dur")
                if dur is not None:
                    state["runDuration"] = dur
                state["globalEnabled"] = qflag(qs, "en")
                state["pumpEnabled"] = qflag(qs, "pen")
                lead = qint(qs, "lead")
                if lead is not None:
                    state["lightLeadMinutes"] = lead
                state["lightEnabled"] = qflag(qs, "len")

                blead = qint(qs, "blead")
                bopen = qint(qs, "bopen")
                bclose = qint(qs, "bclose")
                if blead is not None:
                    state["blindLeadMinutes"] = blead
                if bopen is not None:
                    state["blindOpenDuration"] = bopen
                if bclose is not None:
                    state["blindCloseDuration"] = bclose
                state["blindEnabled"] = qflag(qs, "ben")

                for i in range(6):
                    p = qint(qs, "p%d" % i)
                    if p is not None:
                        state["motorSlowdown"][i] = max(0, min(100, p))

                for i in range(7):
                    h = qint(qs, "h%d" % i)
                    m = qint(qs, "m%d" % i)
                    if h is not None:
                        state["schedule"][i]["hour"] = h
                    if m is not None:
                        state["schedule"][i]["minute"] = m
                    state["schedule"][i]["active"] = qflag(qs, "a%d" % i)

                state["scheduleErrorMsg"] = ""

        elif path == "/BLIND_OPEN":
            cur_pos = current_blind_position()
            if cur_pos == -1:
                cur_pos = 0
            remain_pct = 100 - cur_pos
            if remain_pct > 0:
                state["blindRunStartPos"] = cur_pos
                state["blindManualActive"] = True
                state["blindManualDirection"] = 1
                state["blindRunStartTime"] = time.time()
                state["blindRunFullSec"] = float(state["blindOpenDuration"])
                state["blindRunTotalSec"] = state["blindRunFullSec"] * remain_pct / 100.0
            else:
                state["pendingAnnounceMsg"] = "La tapparella e' gia' aperta"

        elif path == "/BLIND_CLOSE":
            if is_blind_closing_locked():
                state["pendingAnnounceMsg"] = "Tapparella bloccata: finestra di sveglia attiva"
            else:
                cur_pos = current_blind_position()
                if cur_pos == -1:
                    cur_pos = 100
                remain_pct = cur_pos
                if remain_pct > 0:
                    state["blindRunStartPos"] = cur_pos
                    state["blindManualActive"] = True
                    state["blindManualDirection"] = -1
                    state["blindRunStartTime"] = time.time()
                    state["blindRunFullSec"] = float(state["blindCloseDuration"])
                    state["blindRunTotalSec"] = state["blindRunFullSec"] * remain_pct / 100.0
                else:
                    state["pendingAnnounceMsg"] = "La tapparella e' gia' chiusa"

        elif path == "/BLIND_STOP":
            if is_blind_closing_locked():
                state["pendingAnnounceMsg"] = "Tapparella bloccata: finestra di sveglia attiva"
            else:
                if state["blindManualActive"]:
                    state["blindPositionPct"] = current_blind_position()
                state["blindManualActive"] = False
                state["blindManualDirection"] = 0

        elif path == "/BLIND_FORCE_POS":
            if is_blind_closing_locked():
                state["pendingAnnounceMsg"] = "Tapparella bloccata: finestra di sveglia attiva"
            else:
                pos = qint(qs, "pos")
                if pos is not None:
                    state["blindManualActive"] = False
                    state["blindManualDirection"] = 0
                    state["blindPositionPct"] = max(0, min(100, pos))

        elif path == "/SET_SLEEP_DELAY":
            sld = qint(qs, "sld")
            if sld is not None:
                state["fallingAsleepMinutes"] = sld

        elif path == "/CALC_BED":
            wh = qint(qs, "wh")
            wm = qint(qs, "wm")
            if wh is not None and wm is not None:
                state["targetWakeH"] = wh
                state["targetWakeM"] = wm
                state["showBedTimes"] = True

        elif path == "/DEV_SET_TIME":
            mode = qs.get("mode", [""])[0]
            real_now = datetime.now()
            if mode == "abs":
                day = qint(qs, "day", real_now.isoweekday() % 7)
                h = qint(qs, "h", 0)
                m = qint(qs, "m", 0)
                real_day = real_now.isoweekday() % 7
                target = (real_now + timedelta(days=(day - real_day))).replace(
                    hour=h, minute=m, second=0, microsecond=0)
                state["timeOffsetSec"] = (target - real_now).total_seconds()
            elif mode == "rel":
                offset_min = qint(qs, "offset", 0)
                cur_day = real_now.isoweekday() % 7
                sched = state["schedule"][cur_day]
                target = real_now.replace(hour=sched["hour"], minute=sched["minute"],
                                           second=0, microsecond=0) + timedelta(minutes=offset_min)
                state["timeOffsetSec"] = (target - real_now).total_seconds()

        elif path == "/DEV_RESET_TIME":
            state["timeOffsetSec"] = 0

        elif path == "/DEV_OTA_TOGGLE_AVAILABLE":
            state["otaSimulateAvailable"] = not state["otaSimulateAvailable"]

        elif path == "/DEV_OTA_TOGGLE_FAILURE":
            state["otaSimulateFailure"] = not state["otaSimulateFailure"]

        elif path == "/OTA_CHECK":
            if state["otaSimulateAvailable"]:
                state["otaUpdateAvailable"] = True
                state["otaLatestVersion"] = state["firmwareVersion"] + "-new"
            else:
                state["otaUpdateAvailable"] = False
                state["otaLatestVersion"] = ""

        elif path == "/OTA_APPLY":
            if not is_schedule_locked() and not state["blindManualActive"]:
                if state["otaSimulateFailure"]:
                    state["otaState"] = "error"
                    state["otaErrorMsg"] = "OTA download failed (-1) [simulated]"
                else:
                    state["firmwareVersion"] = state["otaLatestVersion"]
                    state["otaUpdateAvailable"] = False
                    state["otaLatestVersion"] = ""
                    state["otaState"] = "idle"

        elif path == "/OTA_DISMISS":
            state["otaState"] = "idle"
            state["otaErrorMsg"] = ""


# --------------------------------------------------------------------------
# HTML rendering (port of web_server.cpp renderers)
# --------------------------------------------------------------------------

DASHBOARD_CSS = (
    "body{font-family:-apple-system,system-ui,sans-serif;text-align:center;margin:0;padding:10px;"
    "background-color:#efeff4;color:#333;}.card{background:white;padding:15px;margin:15px auto;"
    "max-width:500px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.05);}"
    "h2{font-size:1.1rem;margin-top:0;color:#666;border-bottom:1px solid #eee;padding-bottom:8px;"
    "text-transform:uppercase;}h3{font-size:0.9rem;color:#888;margin:15px 0 5px 0;text-align:left;"
    "border-left:3px solid #007aff;padding-left:8px;}"
    ".row{display:flex;align-items:center;justify-content:space-between;padding:10px 0;"
    "border-bottom:1px solid #f0f0f0;}"
    "input[type=number]{padding:8px;width:50px;text-align:center;font-size:16px;border:1px solid #ccc;"
    "border-radius:6px;}"
    ".btn{width:100%;padding:15px;border:none;border-radius:8px;font-size:16px;font-weight:bold;"
    "cursor:pointer;margin-top:10px;}.btn-save{background-color:#007aff;color:white;}"
    ".btn-manual{background-color:#ff3b30;color:white;}.btn-stop{background-color:#8e8e93;color:white;}"
    ".cycle-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px;}"
    ".cycle-item{background:#f8f8f8;padding:10px;border-radius:8px;font-size:0.85rem;}"
    ".cycle-time{font-weight:bold;color:#007aff;display:block;font-size:1.1rem;}"
    ".calc-row{display:flex;align-items:center;justify-content:flex-start;gap:5px;padding:10px 0;}"
    ".error-msg{background:#ff3b30;color:white;padding:12px;border-radius:8px;margin:15px auto;"
    "max-width:500px;font-weight:bold;}"
    ".prow{display:flex;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid #f5f5f5;}"
    ".plbl{width:36px;font-size:0.82rem;color:#666;text-align:right;flex-shrink:0;}"
    ".prow input[type=range]{flex:1;min-width:0;height:28px;cursor:pointer;}"
    ".pval{width:38px;font-size:0.9rem;font-weight:bold;color:#007aff;text-align:right;flex-shrink:0;}"
    ".devcard{border:2px dashed #ff9500;}"
)


def checked(b):
    return "checked" if b else ""


def render_pin_page(error_msg="", locked=False):
    html = []
    html.append("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">")
    html.append("<style>body{font-family:-apple-system,system-ui,sans-serif;text-align:center;margin:0;"
                 "padding:40px 10px;background-color:#efeff4;}.card{background:white;padding:30px;"
                 "margin:60px auto;max-width:300px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.05);}"
                 "h2{font-size:1.2rem;color:#333;margin-bottom:20px;}"
                 "#pinInput{padding:12px;width:100%;text-align:center;font-size:26px;letter-spacing:10px;"
                 "border:1px solid #ccc;border-radius:6px;box-sizing:border-box;margin-bottom:15px;"
                 "caret-color:transparent;}"
                 ".error{background:#ff3b30;color:white;padding:10px;border-radius:8px;margin-bottom:15px;"
                 "font-size:0.9rem;}</style>")
    html.append("</head><body><div class=\"card\"><h2>ClockHub</h2>")
    if error_msg:
        html.append("<div class=\"error\">" + error_msg + "</div>")
    else:
        html.append("<p style=\"color:#888;font-size:0.9rem;margin-bottom:15px\">Enter PIN to access</p>")
    if not locked:
        html.append("<form id=\"pinForm\" action=\"/\" method=\"GET\">")
        html.append("<input type=\"text\" id=\"pinInput\" inputmode=\"numeric\" placeholder=\"PIN\" "
                     "autocomplete=\"off\" autocorrect=\"off\" autocapitalize=\"off\" spellcheck=\"false\" autofocus>")
        html.append("<input type=\"hidden\" name=\"pin\" id=\"pinHidden\">")
        html.append("</form>")
        html.append("<script>")
        html.append("var pin='',timer=null,dot=String.fromCharCode(8226);")
        html.append("var inp=document.getElementById('pinInput');")
        html.append("var hid=document.getElementById('pinHidden');")
        html.append("inp.addEventListener('input',function(){")
        html.append("  var raw=this.value;")
        html.append("  if(raw.length>pin.length){")
        html.append("    pin+=raw.slice(pin.length).replace(/[^0-9]/g,'');")
        html.append("  } else {")
        html.append("    pin=pin.slice(0,raw.length);")
        html.append("  }")
        html.append("  this.value=dot.repeat(pin.length);")
        html.append("  hid.value=pin;")
        html.append("  clearTimeout(timer);")
        html.append("  if(pin.length>0) timer=setTimeout(function(){document.getElementById('pinForm').submit();},1500);")
        html.append("});")
        html.append("</script>")
    html.append("</div></body></html>")
    return "".join(html)


def locked_button(label, schedule_locked):
    if schedule_locked:
        return ("<button type=\"button\" class=\"btn\" style=\"background:#ccc;cursor:not-allowed\" "
                "disabled>" + label + "</button>")
    return "<button type=\"submit\" class=\"btn btn-save\">" + label + "</button>"


def render_dev_card(pin_param):
    s = state
    html = []
    html.append("<div class=\"card devcard\"><h2>Dev Tools (mock only)</h2>")
    html.append("<p style=\"margin:0 0 10px;color:#666\">Simulated time: <b>"
                 + DAYS_OF_WEEK[current_day()] + " " + formatted_time() + "</b>"
                 + (" <span style='color:#ff9500'>(offset active)</span>" if s["timeOffsetSec"] else "")
                 + "</p>")

    html.append("<form action=\"/DEV_SET_TIME\" method=\"GET\" style=\"display:flex;gap:6px;align-items:center;"
                 "justify-content:center;flex-wrap:wrap\">")
    html.append("<input type=\"hidden\" name=\"pin\" value=\"" + pin_param + "\">")
    html.append("<input type=\"hidden\" name=\"mode\" value=\"abs\">")
    html.append("<select name=\"day\">")
    for i, d in enumerate(DAYS_OF_WEEK):
        html.append("<option value=\"%d\"%s>%s</option>" % (i, " selected" if i == current_day() else "", d))
    html.append("</select>")
    now = now_dt()
    html.append("<input type=\"number\" name=\"h\" min=\"0\" max=\"23\" value=\"%d\"> : "
                 "<input type=\"number\" name=\"m\" min=\"0\" max=\"59\" value=\"%d\">" % (now.hour, now.minute))
    html.append("<button type=\"submit\" class=\"btn btn-save\" style=\"width:auto;margin:0;padding:8px 14px\">Set</button>")
    html.append("</form>")

    html.append("<div style=\"display:flex;gap:6px;flex-wrap:wrap;margin-top:10px;justify-content:center\">")
    for label, offset in (("-65 min", -65), ("-31 min", -31), ("-15 min", -15), ("Alarm", 0), ("+35 min", 35)):
        html.append("<a href=\"/DEV_SET_TIME?mode=rel&offset=%d&%s\">"
                     "<button type=\"button\" class=\"btn\" style=\"width:auto;margin:0;padding:8px 12px;"
                     "background:#eee\">%s</button></a>" % (offset, pin_param, label))
    html.append("<a href=\"/DEV_RESET_TIME?%s\"><button type=\"button\" class=\"btn\" "
                 "style=\"width:auto;margin:0;padding:8px 12px;background:#eee\">Real time</button></a>" % pin_param)
    html.append("</div>")
    html.append("<p style=\"font-size:0.72rem;color:#999;margin:8px 0 0\">Quick jumps are relative to "
                 "<b>today's</b> configured alarm time.</p>")

    html.append("<div style=\"display:flex;gap:6px;flex-wrap:wrap;margin-top:10px;justify-content:center\">")
    html.append("<a href=\"/DEV_OTA_TOGGLE_AVAILABLE?%s\"><button type=\"button\" class=\"btn\" "
                 "style=\"width:auto;margin:0;padding:8px 12px;background:%s\">Simulate update available: %s"
                 "</button></a>" % (pin_param,
                                     "#34c759" if state["otaSimulateAvailable"] else "#eee",
                                     "ON" if state["otaSimulateAvailable"] else "OFF"))
    html.append("<a href=\"/DEV_OTA_TOGGLE_FAILURE?%s\"><button type=\"button\" class=\"btn\" "
                 "style=\"width:auto;margin:0;padding:8px 12px;background:%s\">Simulate apply failure: %s"
                 "</button></a>" % (pin_param,
                                     "#ff3b30" if state["otaSimulateFailure"] else "#eee",
                                     "ON" if state["otaSimulateFailure"] else "OFF"))
    html.append("</div>")
    html.append("<p style=\"font-size:0.72rem;color:#999;margin:8px 0 0\">These two toggles affect the "
                 "Firmware card's Check/Update buttons below.</p>")

    html.append("</div>")
    return "".join(html)


def render_schedule_card(schedule_locked):
    disabled = " disabled" if schedule_locked else ""
    html = ["<div class=\"card\"><h2>Weekly Schedule</h2>"]
    for i in range(7):
        sched = state["schedule"][i]
        html.append(
            "<div class=\"row\"><b>" + DAYS_OF_WEEK[i] + "</b><div>"
            + "<input type=\"number\" name=\"h%d\" min=\"0\" max=\"23\" value=\"%d\"%s> : "
              % (i, sched["hour"], disabled)
            + "<input type=\"number\" name=\"m%d\" min=\"0\" max=\"59\" value=\"%d\"%s>"
              % (i, sched["minute"], disabled)
            + "<input type=\"checkbox\" name=\"a%d\" %s style=\"width:20px;height:20px;margin-left:8px\"%s>"
              % (i, checked(sched["active"]), disabled)
            + "</div></div>")
    html.append("</div>")
    return "".join(html)


def render_settings_card(schedule_locked):
    disabled = " disabled" if schedule_locked else ""
    return ("<div class=\"card\"><h2>Settings</h2>"
            "<div class=\"row\"><span>System Enabled</span>"
            "<input type=\"checkbox\" name=\"en\" %s style=\"width:20px;height:20px\"%s></div>"
            "</div>" % (checked(state["globalEnabled"]), disabled))


def render_pump_card(schedule_locked):
    disabled = " disabled" if schedule_locked else ""
    return ("<div class=\"card\"><h2>Pump Settings</h2>"
            "<div class=\"row\"><span>Enabled</span>"
            "<input type=\"checkbox\" name=\"pen\" %s style=\"width:20px;height:20px\"%s></div>"
            "<div class=\"row\"><span>Duration (sec)</span>"
            "<input type=\"number\" name=\"dur\" value=\"%d\"%s></div>"
            "</div>" % (checked(state["pumpEnabled"]), disabled, state["runDuration"], disabled))


def render_light_card(schedule_locked):
    disabled = " disabled" if schedule_locked else ""
    return ("<div class=\"card\"><h2>Light Settings</h2>"
            "<div class=\"row\"><span>Enabled</span>"
            "<input type=\"checkbox\" name=\"len\" %s style=\"width:20px;height:20px\"%s></div>"
            "<div class=\"row\"><span>Lead Time (minutes)</span>"
            "<input type=\"number\" name=\"lead\" value=\"%d\"%s></div>"
            "</div>" % (checked(state["lightEnabled"]), disabled, state["lightLeadMinutes"], disabled))


def render_sleep_card(hp):
    opts = (10.5, 9.0, 7.5, 6.0)
    html = []
    html.append("<div class=\"card\"><h2>Sleep Cycles</h2>"
                 "<form action=\"/SET_SLEEP_DELAY\" method=\"GET\">" + hp
                 + "<div class=\"row\" style=\"border-bottom:1px solid #eee\"><span>Time to fall asleep (min)</span>"
                 + "<div style=\"display:flex;gap:5px\"><input type=\"number\" name=\"sld\" value=\"%d\" min=\"0\">"
                   % state["fallingAsleepMinutes"]
                 + "<button type=\"submit\" style=\"padding:8px; border-radius:6px; border:none; "
                   "background:#007aff; color:white\">OK</button>"
                 + "</div></div></form>"
                 + "<h3>If you go to sleep now</h3><div class=\"cycle-grid\">")
    for o in opts:
        html.append("<div class=\"cycle-item\">%.1fh (%d cycles)<span class=\"cycle-time\">%s</span></div>"
                     % (o, int(o / 1.5), get_wake_time(o)))
    html.append("</div><h3>If you want to wake up at...</h3>"
                 "<form action=\"/CALC_BED\" method=\"GET\">" + hp
                 + "<div class=\"calc-row\">"
                 + "<input type=\"number\" name=\"wh\" value=\"%d\" min=\"0\" max=\"23\"> : " % state["targetWakeH"]
                 + "<input type=\"number\" name=\"wm\" value=\"%d\" min=\"0\" max=\"59\">" % state["targetWakeM"]
                 + "<button type=\"submit\" style=\"padding:10px 15px; border-radius:6px; border:none; "
                   "background:#007aff; color:white; font-weight:bold\">CALCULATE</button>"
                 + "</div></form>")
    if state["showBedTimes"]:
        html.append("<div class=\"cycle-grid\">")
        for o in opts:
            html.append("<div class=\"cycle-item\">Sleep %.1fh<span class=\"cycle-time\">%s</span></div>"
                         % (o, get_bed_time(state["targetWakeH"], state["targetWakeM"], o)))
        html.append("</div>")
    html.append("</div>")
    return "".join(html)


def render_blind_card(pin_param):
    if state["blindManualActive"] and state["blindManualDirection"] == 1:
        state_label, state_color = "OPENING", "#34c759"
    elif state["blindManualActive"] and state["blindManualDirection"] == -1:
        state_label, state_color = "CLOSING", "#ff9500"
    else:
        state_label, state_color = "IDLE", "#8e8e93"

    pos = current_blind_position()
    pos_str = "unknown" if pos < 0 else ("%d%% open" % pos)

    html = []
    html.append("<div class=\"card\"><h2>Blind</h2>")
    html.append("<p style=\"font-weight:bold;color:%s;margin-bottom:2px\">%s</p>" % (state_color, state_label))
    html.append("<p style=\"margin:0 0 10px;color:#555\">Position: <b>%s</b></p>" % pos_str)
    html.append("<div style=\"display:flex;gap:8px\">")
    html.append("<a href=\"/BLIND_OPEN?%s\" style=\"flex:1\"><button class=\"btn\" "
                 "style=\"background:#34c759;color:white;margin:0\">OPEN</button></a>" % pin_param)
    html.append("<a href=\"/BLIND_CLOSE?%s\" style=\"flex:1\"><button class=\"btn\" "
                 "style=\"background:#ff9500;color:white;margin:0\">CLOSE</button></a>" % pin_param)
    html.append("<a href=\"/BLIND_STOP?%s\" style=\"flex:1\"><button class=\"btn btn-stop\" "
                 "style=\"margin:0\">STOP</button></a>" % pin_param)
    html.append("</div>")
    html.append("<div style=\"margin-top:14px;padding-top:12px;border-top:1px solid #f0f0f0\">")
    html.append("<p style=\"color:#bbb;font-size:0.75rem;margin:0 0 8px\">Position override (motor does not move)</p>")
    html.append("<div style=\"display:flex;gap:8px\">")
    html.append("<a href=\"/BLIND_FORCE_POS?%s&pos=0\" "
                 "onclick=\"return confirm('Force position to CLOSED (0%% open)? The motor will not move.')\" "
                 "style=\"flex:1\">" % pin_param)
    html.append("<button class=\"btn\" style=\"background:#ff3b30;color:white;margin:0;font-size:0.88rem;"
                 "padding:10px\">Force Closed</button></a>")
    html.append("<a href=\"/BLIND_FORCE_POS?%s&pos=100\" "
                 "onclick=\"return confirm('Force position to OPEN (100%% open)? The motor will not move.')\" "
                 "style=\"flex:1\">" % pin_param)
    html.append("<button class=\"btn\" style=\"background:#34c759;color:white;margin:0;font-size:0.88rem;"
                 "padding:10px\">Force Open</button></a>")
    html.append("</div></div></div>")
    return "".join(html)


def render_blind_settings_card(schedule_locked):
    disabled = " disabled" if schedule_locked else ""
    html = []
    html.append("<div class=\"card\"><h2>Blind Settings</h2>")
    html.append("<div class=\"row\"><span>Enabled</span>"
                 "<input type=\"checkbox\" name=\"ben\" %s style=\"width:20px;height:20px\"%s></div>"
                 % (checked(state["blindEnabled"]), disabled))
    html.append("<div class=\"row\"><span>Open Lead Time (min)</span>"
                 "<input type=\"number\" name=\"blead\" value=\"%d\" min=\"0\"%s></div>"
                 % (state["blindLeadMinutes"], disabled))
    html.append("<div class=\"row\"><span>Open Duration (sec)</span>"
                 "<input type=\"number\" name=\"bopen\" value=\"%d\" min=\"1\"%s></div>"
                 % (state["blindOpenDuration"], disabled))
    html.append("<div class=\"row\"><span>Close Duration (sec)</span>"
                 "<input type=\"number\" name=\"bclose\" value=\"%d\" min=\"1\"%s></div>"
                 % (state["blindCloseDuration"], disabled))
    html.append("<h3>End-of-run Slowdown (last 30%)</h3>")
    html.append("<p style=\"font-size:0.75rem;color:#999;text-align:left;margin:2px 0 10px\">"
                 "First 70% always at full power &mdash; applies to scheduled runs</p>")
    pct_labels = ["70%", "75%", "80%", "85%", "90%", "95%"]
    for i in range(6):
        val = state["motorSlowdown"][i]
        html.append(
            "<div class=\"prow\">"
            "<span class=\"plbl\">%s</span>"
            "<input type=\"range\" name=\"p%d\" min=\"0\" max=\"100\" value=\"%d\"%s "
            "oninput=\"document.getElementById('pv%d').textContent=this.value+'%%'\">"
            "<span class=\"pval\" id=\"pv%d\">%d%%</span>"
            "</div>" % (pct_labels[i], i, val, disabled, i, i, val))
    html.append("<p style=\"font-size:0.72rem;color:#bbb;text-align:right;margin:4px 0 0\">At 100%: motor stops</p>")
    html.append("</div>")
    return "".join(html)


def render_firmware_card(pin_param, schedule_locked):
    html = []
    html.append("<div class=\"card\"><h2>Firmware</h2>")
    html.append("<div class=\"row\"><span>Current version</span><b>%s</b></div>" % state["firmwareVersion"])

    if state["otaState"] == "error":
        html.append("<div class=\"error-msg\" style=\"margin:15px 0\">%s</div>" % state["otaErrorMsg"])
        html.append("<a href=\"/OTA_DISMISS?%s\"><button class=\"btn btn-stop\">Dismiss</button></a>" % pin_param)
    elif state["otaUpdateAvailable"]:
        html.append("<div class=\"row\"><span>Latest version</span>"
                     "<b style=\"color:#34c759\">%s</b></div>" % state["otaLatestVersion"])
        if schedule_locked or state["blindManualActive"]:
            html.append("<button type=\"button\" class=\"btn\" style=\"background:#ccc;cursor:not-allowed\" "
                         "disabled>UPDATE NOW</button>")
        else:
            html.append("<a href=\"/OTA_APPLY?%s\" onclick=\"return confirm('Aggiornare il firmware ora? "
                         "Il sistema si bloccherà per circa un minuto e poi il dispositivo si "
                         "riavvierà.')\">" % pin_param)
            html.append("<button class=\"btn btn-manual\">UPDATE NOW</button></a>")
    else:
        html.append("<p style=\"color:#8e8e93;font-size:0.85rem;margin:10px 0 0\">Firmware up to date</p>")

    html.append("<a href=\"/OTA_CHECK?%s\"><button class=\"btn btn-stop\" "
                 "style=\"margin-top:8px\">Check for updates</button></a>" % pin_param)
    html.append("</div>")
    return "".join(html)


def render_status_card(pin_param):
    pump_running = state["manualOverride"] or (time.time() < state["pumpOnUntil"])
    html = []
    html.append("<div class=\"card\"><h2>Status</h2>")
    html.append("<p>%s, %s</p>" % (DAYS_OF_WEEK[current_day()], formatted_time()))
    html.append("<p style=\"font-weight:bold\">")
    if state["manualOverride"]:
        html.append("<span style='color:#ff3b30'>MANUAL ON</span>")
    else:
        html.append("RUNNING" if pump_running else "IDLE")
    html.append("</p>")
    onclick = "" if state["manualOverride"] else " onclick=\"return confirm('Confirm manual pump start?')\""
    btn_class = "btn-stop" if state["manualOverride"] else "btn-manual"
    btn_label = "STOP PUMP" if state["manualOverride"] else "MANUAL START"
    html.append("<a href=\"/TOGGLE?%s\"%s><button class=\"btn %s\">%s</button></a></div></body></html>"
                 % (pin_param, onclick, btn_class, btn_label))
    return "".join(html)


def render_dashboard(pin_param, schedule_locked):
    hp = "<input type=\"hidden\" name=\"pin\" value=\"%s\">" % pin_param
    html = []
    html.append("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">")
    html.append("<style>" + DASHBOARD_CSS + "</style>")

    html.append("<script>")
    html.append("var currentLockState = " + ("true" if schedule_locked else "false") + ";")
    html.append("setInterval(function() {")
    html.append("  fetch('/CHECK_LOCK').then(r => r.text()).then(state => {")
    html.append("    var newLockState = (state === '1');")
    html.append("    if (newLockState !== currentLockState) location.reload();")
    html.append("  });")
    html.append("}, 5000);")
    html.append("</script>")

    html.append("</head><body>")

    html.append(render_dev_card(pin_param))

    if schedule_locked and state["scheduleErrorMsg"]:
        html.append("<div class=\"error-msg\">" + state["scheduleErrorMsg"] + "</div>")

    html.append("<form action=\"/SAVE_ALL\" method=\"GET\">" + hp)
    html.append(render_schedule_card(schedule_locked))
    html.append(render_settings_card(schedule_locked))
    html.append(render_pump_card(schedule_locked))
    html.append(render_light_card(schedule_locked))
    html.append(render_blind_settings_card(schedule_locked))
    html.append("<div class=\"card\" style=\"padding:10px\">")
    html.append(locked_button("SAVE ALL SETTINGS", schedule_locked))
    html.append("</div></form>")
    html.append(render_sleep_card(hp))
    html.append(render_blind_card(pin_param))
    html.append(render_firmware_card(pin_param, schedule_locked))
    html.append(render_status_card(pin_param))
    return "".join(html)


# --------------------------------------------------------------------------
# HTTP server
# --------------------------------------------------------------------------

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, status, content_type, body, extra_headers=None):
        encoded = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-type", content_type)
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self):
        global pin_fail_count, pin_first_fail_time

        tick()

        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)
        pin_param = qs.get("pin", [""])[0]
        is_pin_valid = (pin_param == ACCESS_PIN)

        if path == "/LOGIN":
            if is_pin_valid:
                self._send(302, "text/html", "",
                           extra_headers={"Location": "/?pin=" + ACCESS_PIN})
            else:
                self._send(200, "text/html", render_pin_page())
            return

        if path != "/CHECK_LOCK" and not is_pin_valid:
            now = time.time()
            if pin_fail_count > 0 and (now - pin_first_fail_time) >= PIN_LOCKOUT_SEC:
                pin_fail_count = 0

            has_pin = "pin" in qs
            if pin_fail_count >= PIN_MAX_ATTEMPTS:
                mins_left = int((PIN_LOCKOUT_SEC - (now - pin_first_fail_time)) // 60) + 1
                self._send(200, "text/html",
                           render_pin_page("Too many attempts. Try again in %d min." % mins_left, locked=True))
            elif has_pin:
                if pin_fail_count == 0:
                    pin_first_fail_time = now
                pin_fail_count += 1
                remaining = PIN_MAX_ATTEMPTS - pin_fail_count
                if remaining > 0:
                    self._send(200, "text/html",
                               render_pin_page("Incorrect PIN. Remaining attempts: %d" % remaining))
                else:
                    self._send(200, "text/html",
                               render_pin_page("Too many attempts. Try again in 5 min.", locked=True))
            else:
                self._send(200, "text/html", render_pin_page())
            return

        pin_fail_count = 0

        if path == "/CHECK_LOCK":
            self._send(200, "text/plain", "1" if is_schedule_locked() else "0",
                       extra_headers={"Cache-Control": "no-store"})
            return

        handle_routes(path, qs)
        schedule_locked = is_schedule_locked()
        self._send(200, "text/html", render_dashboard(pin_param, schedule_locked),
                   extra_headers={"Cache-Control": "no-store"})


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def get_lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    except OSError:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    server = Server(("0.0.0.0", port), Handler)
    lan_ip = get_lan_ip()
    print("ClockHub mock server running")
    print("  Local:   http://127.0.0.1:%d/?pin=%s" % (port, ACCESS_PIN))
    print("  Network: http://%s:%d/?pin=%s" % (lan_ip, port, ACCESS_PIN))
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
