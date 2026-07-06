#include "web_server.h"
#include "globals.h"
#include "scheduler.h"
#include "storage.h"
#include "time_utils.h"
#include "ota_manager.h"
#include "config.h"
#include <Arduino.h>

static int pinFailCount = 0;
static unsigned long pinFirstFailTime = 0;
static const int PIN_MAX_ATTEMPTS = 5;
static const unsigned long PIN_LOCKOUT_MS = 300000UL;

static String parseParam(const String& req, const String& key, char terminator = '&') {
  int idx = req.indexOf(key);
  if (idx < 0) return "";
  int start = idx + key.length();
  int end = req.indexOf(terminator, start);
  if (end < 0) end = req.indexOf(' ', start);
  if (end < 0) return req.substring(start);
  return req.substring(start, end);
}

class BufferedClient {
public:
  explicit BufferedClient(WiFiClient& c) : client(c), len(0) {}
  void print(const String& s) { write(s.c_str(), s.length()); }
  void println(const String& s) { print(s); write("\r\n", 2); }
  void println() { write("\r\n", 2); }
  void flush() {
    if (len > 0) {
      client.write((const uint8_t*)buf, len);
      len = 0;
    }
  }
private:
  void write(const char* data, size_t n) {
    while (n > 0) {
      size_t space = sizeof(buf) - len;
      size_t chunk = (n < space) ? n : space;
      memcpy(buf + len, data, chunk);
      len += chunk;
      data += chunk;
      n -= chunk;
      if (len == sizeof(buf)) flush();
    }
  }
  WiFiClient& client;
  static char buf[2048];
  size_t len;
};

char BufferedClient::buf[2048];

static String extractPinValue(const String& source, const String& key) {
  int idx = source.indexOf(key);
  if (idx < 0) return "";
  int start = idx + key.length();
  int end = start;
  while (end < (int)source.length()) {
    char c = source.charAt(end);
    if (c == '&' || c == ' ' || c == ';' || c == '\r' || c == '\n') break;
    end++;
  }
  return source.substring(start, end);
}

static String authCookieHeader() {
  return "Set-Cookie: pin=" + String(access_pin) + "; Max-Age=" + String(AUTH_COOKIE_MAX_AGE_SEC) + "; Path=/; HttpOnly; SameSite=Lax";
}

static String toggleSwitch(const String& name, bool isChecked, const String& disabledAttr, const String& extraStyle = "") {
  return "<label class=\"switch\"" + (extraStyle.length() > 0 ? " style=\"" + extraStyle + "\"" : "") + ">"
    + "<input type=\"checkbox\" name=\"" + name + "\" " + (isChecked ? "checked" : "") + disabledAttr + ">"
    + "<span class=\"slider\"></span></label>";
}

static void lockedButton(BufferedClient& client, const String& label, bool scheduleLocked) {
  if (scheduleLocked) {
    client.println("<button type=\"button\" class=\"btn\" style=\"background:#ccc;cursor:not-allowed\" disabled>" + label + "</button>");
  } else {
    client.println("<button type=\"submit\" class=\"btn btn-save\">" + label + "</button>");
  }
}

static void servePinPage(BufferedClient& client, const String& errorMsg = "", bool locked = false, bool clearStaleCookie = false) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  if (clearStaleCookie) {
    client.println("Set-Cookie: pin=; Max-Age=0; Path=/");
  }
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><link rel=\"icon\" href=\"data:,\">");
  client.println("<style>body{font-family:-apple-system,system-ui,sans-serif;text-align:center;margin:0;padding:40px 10px;background-color:#efeff4;}.card{background:white;padding:30px;margin:60px auto;max-width:300px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.05);}h2{font-size:1.2rem;color:#333;margin-bottom:20px;}#pinInput{padding:12px;width:100%;text-align:center;font-size:26px;letter-spacing:10px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;margin-bottom:15px;caret-color:transparent;}.error{background:#ff3b30;color:white;padding:10px;border-radius:8px;margin-bottom:15px;font-size:0.9rem;}</style>");
  client.println("</head><body><div class=\"card\"><h2>ClockHub</h2>");
  if (errorMsg.length() > 0) {
    client.println("<div class=\"error\">" + errorMsg + "</div>");
  } else {
    client.println("<p style=\"color:#888;font-size:0.9rem;margin-bottom:15px\">Enter PIN to access</p>");
  }
  if (!locked) {
    client.println("<form id=\"pinForm\" action=\"/\" method=\"GET\">");
    client.println("<input type=\"text\" id=\"pinInput\" inputmode=\"numeric\" placeholder=\"PIN\" autocomplete=\"off\" autocorrect=\"off\" autocapitalize=\"off\" spellcheck=\"false\" autofocus>");
    client.println("<input type=\"hidden\" name=\"pin\" id=\"pinHidden\">");
    client.println("</form>");
    client.println("<script>");
    client.println("var pin='',timer=null,dot=String.fromCharCode(8226);");
    client.println("var inp=document.getElementById('pinInput');");
    client.println("var hid=document.getElementById('pinHidden');");
    client.println("inp.addEventListener('input',function(){");
    client.println("  var raw=this.value;");
    client.println("  if(raw.length>pin.length){");
    client.println("    pin+=raw.slice(pin.length).replace(/[^0-9]/g,'');");
    client.println("  } else {");
    client.println("    pin=pin.slice(0,raw.length);");
    client.println("  }");
    client.println("  this.value=dot.repeat(pin.length);");
    client.println("  hid.value=pin;");
    client.println("  clearTimeout(timer);");
    client.println("  if(pin.length>0) timer=setTimeout(function(){document.getElementById('pinForm').submit();},1500);");
    client.println("});");
    client.println("</script>");
  }
  client.println("</div></body></html>");
}

// Returns true if the request matched an action route (so the caller should
// redirect to the clean dashboard URL instead of rendering it as the response
// to the action URL itself) — false for a plain dashboard load (no route
// matched). Redirecting afterwards keeps the browser's address bar/history on
// "/?pin=..." instead of e.g. "/ARM_ONESHOT?...&osH=0&osM=20...", so a later
// reload (manual, or the dashboard's own /CHECK_LOCK polling) can never
// silently replay an already-handled action — this previously caused a
// one-shot alarm to immediately re-arm itself after firing, since
// disarming changes the lock state, which triggers the polling reload.
static bool handleRoutes(const String& request) {
  bool matched = request.indexOf("GET /TOGGLE") >= 0
    || request.indexOf("GET /SAVE_ALL") >= 0
    || request.indexOf("GET /ARM_ONESHOT") >= 0
    || request.indexOf("GET /CANCEL_ONESHOT") >= 0
    || request.indexOf("GET /BLIND_OPEN") >= 0
    || request.indexOf("GET /BLIND_CLOSE") >= 0
    || request.indexOf("GET /BLIND_STOP") >= 0
    || request.indexOf("GET /BLIND_FORCE_POS") >= 0
    || request.indexOf("GET /SET_SLEEP_DELAY") >= 0
    || request.indexOf("GET /CALC_BED") >= 0
    || request.indexOf("GET /OTA_CHECK") >= 0
    || request.indexOf("GET /OTA_DISMISS") >= 0;

  if (request.indexOf("GET /TOGGLE") >= 0) {
    manualOverride = !manualOverride;
    if (!manualOverride) digitalWrite(PUMP_PIN, LOW);
  }
  else if (request.indexOf("GET /SAVE_ALL") >= 0) {
    if (!isScheduleLocked()) {
      String durVal = parseParam(request, "dur=");
      if (durVal.length() > 0) sysConfig.runDuration = durVal.toInt();
      sysConfig.globalEnabled = (request.indexOf("&en=") >= 0);
      sysConfig.pumpEnabled = (request.indexOf("&pen=") >= 0);
      String leadVal = parseParam(request, "lead=");
      if (leadVal.length() > 0) sysConfig.lightLeadMinutes = leadVal.toInt();
      sysConfig.lightEnabled = (request.indexOf("&len=") >= 0);
      String bldLead = parseParam(request, "blead=");
      String bldOpen = parseParam(request, "bopen=");
      String bldClose = parseParam(request, "bclose=");
      if (bldLead.length() > 0)  sysConfig.blindLeadMinutes   = bldLead.toInt();
      if (bldOpen.length() > 0)  sysConfig.blindOpenDuration  = bldOpen.toInt();
      if (bldClose.length() > 0) sysConfig.blindCloseDuration = bldClose.toInt();
      sysConfig.blindEnabled = (request.indexOf("&ben=") >= 0);
      for (int i = 0; i < 6; i++) {
        String pVal = parseParam(request, "p" + String(i) + "=");
        if (pVal.length() > 0) sysConfig.motorSlowdown[i] = (uint8_t)constrain(pVal.toInt(), 0, 100);
      }
      int getLineEnd = request.indexOf('\n');
      String getLine = (getLineEnd > 0) ? request.substring(0, getLineEnd) : request;
      for (int i = 0; i < 7; i++) {
        String hK = "h" + String(i) + "=";
        String mK = "m" + String(i) + "=";
        String aK = "a" + String(i) + "=";
        String hVal = parseParam(getLine, hK);
        String mVal = parseParam(getLine, mK);
        if (hVal.length() > 0) sysConfig.schedule[i].hour   = hVal.toInt();
        if (mVal.length() > 0) sysConfig.schedule[i].minute = mVal.toInt();
        sysConfig.schedule[i].active = (getLine.indexOf(aK) >= 0);
      }
      saveConfig();
      scheduleErrorMsg = "";
    }
  }
  else if (request.indexOf("GET /ARM_ONESHOT") >= 0) {
    // !oneShot.armed guards against a stale page (e.g. a second tab still
    // showing the arm form) silently clobbering an already-armed one-shot.
    if (!isScheduleLocked() && !oneShot.armed) {
      int hoursFromNow   = parseParam(request, "osH=").toInt();
      int minutesFromNow = parseParam(request, "osM=").toInt();
      int totalMin = hoursFromNow * 60 + minutesFromNow;
      bool pE = (request.indexOf("&ospen=") >= 0);
      bool lE = (request.indexOf("&oslen=") >= 0);
      bool bE = (request.indexOf("&osben=") >= 0);
      if (totalMin > 0 && (pE || lE || bE)) {
        oneShot.armed        = true;
        oneShot.triggerEpoch = timeClient.getEpochTime() + (unsigned long)totalMin * 60UL;
        oneShot.pumpEnabled  = pE;
        oneShot.lightEnabled = lE;
        oneShot.blindEnabled = bE;
        String lLead = parseParam(request, "oslead=");
        String bLead = parseParam(request, "osblead=");
        if (lLead.length() > 0) oneShot.lightLeadMinutes = lLead.toInt();
        if (bLead.length() > 0) oneShot.blindLeadMinutes = bLead.toInt();
        oneShot.pumpDone = oneShot.lightDone = oneShot.blindDone = false;
        saveOneShot();
      }
    }
  }
  else if (request.indexOf("GET /CANCEL_ONESHOT") >= 0) {
    if (oneShot.armed) {
      oneShot.armed = false;
      oneShot.pumpDone = oneShot.lightDone = oneShot.blindDone = false;
      saveOneShot();
    }
  }
  else if (request.indexOf("GET /BLIND_OPEN") >= 0) {
    int curPos    = currentBlindPosition();
    if (curPos == -1) curPos = 0;
    int remainPct = 100 - curPos;
    if (remainPct > 0) {
      if (blindManualActive && blindManualDirection == -1) delay(300);
      blindRunStartPos     = curPos;
      blindManualActive    = true;
      blindManualDirection = 1;
      blindRunStartMs      = millis();
      blindRunFullMs       = (unsigned long)sysConfig.blindOpenDuration * 1000UL;
      blindRunTotalMs      = blindRunFullMs * (unsigned long)remainPct / 100UL;
    } else {
      pendingAnnounceMsg   = "The blind is already open";
    }
  }
  else if (request.indexOf("GET /BLIND_CLOSE") >= 0) {
    if (isBlindClosingLocked()) {
      pendingAnnounceMsg = "Blind locked: alarm window active";
    } else {
      int curPos    = currentBlindPosition();
      if (curPos == -1) curPos = 100;
      int remainPct = curPos;
      if (remainPct > 0) {
        if (blindManualActive && blindManualDirection == 1) delay(300);
        blindRunStartPos     = curPos;
        blindManualActive    = true;
        blindManualDirection = -1;
        blindRunStartMs      = millis();
        blindRunFullMs       = (unsigned long)sysConfig.blindCloseDuration * 1000UL;
        blindRunTotalMs      = blindRunFullMs * (unsigned long)remainPct / 100UL;
      } else {
        pendingAnnounceMsg   = "The blind is already closed";
      }
    }
  }
  else if (request.indexOf("GET /BLIND_STOP") >= 0) {
    if (isBlindClosingLocked()) {
      pendingAnnounceMsg = "Blind locked: alarm window active";
    } else {
      if (blindManualActive) {
        blindPositionPct = currentBlindPosition();
        saveBlindPosition();
      }
      blindManualActive    = false;
      blindManualDirection = 0;
    }
  }
  else if (request.indexOf("GET /BLIND_FORCE_POS") >= 0) {
    if (isBlindClosingLocked()) {
      pendingAnnounceMsg = "Blind locked: alarm window active";
    } else {
      String posVal = parseParam(request, "pos=");
      if (posVal.length() > 0) {
        blindManualActive    = false;
        blindManualDirection = 0;
        blindPositionPct     = constrain(posVal.toInt(), 0, 100);
        saveBlindPosition();
      }
    }
  }
  else if (request.indexOf("GET /SET_SLEEP_DELAY") >= 0) {
    String sldVal = parseParam(request, "sld=", ' ');
    if (sldVal.length() > 0) sysConfig.fallingAsleepMinutes = sldVal.toInt();
    saveConfig();
  }
  else if (request.indexOf("GET /CALC_BED") >= 0) {
    String whVal = parseParam(request, "wh=");
    String wmVal = parseParam(request, "wm=", ' ');
    if (whVal.length() > 0 && wmVal.length() > 0) {
      targetWakeH  = whVal.toInt();
      targetWakeM  = wmVal.toInt();
      showBedTimes = true;
    }
  }
  else if (request.indexOf("GET /OTA_CHECK") >= 0) {
    if (otaCheckBusy()) {
      otaCheckNote = "Device busy (pump or blind running). Try again shortly.";
    } else {
      checkForUpdateNow(2);
    }
  }
  // NOTE: GET /OTA_APPLY is handled directly in handleWebRequest() (it must send
  // its response and close the socket BEFORE the blocking OTA), not here.
  else if (request.indexOf("GET /OTA_DISMISS") >= 0) {
    otaState = OTA_IDLE;
    otaErrorMsg = "";
    otaCheckNote = "";
  }

  return matched;
}

static void renderScheduleCard(BufferedClient& client, bool scheduleLocked) {
  String disabledAttr = scheduleLocked ? " disabled" : "";
  client.println("<div class=\"card\"><h2>Weekly Schedule</h2>");
  for (int i = 0; i < 7; i++) {
    client.println("<div class=\"row\"><b>" + String(daysOfWeek[i]) + "</b><div>"
      + "<input type=\"number\" name=\"h" + String(i) + "\" min=\"0\" max=\"23\" value=\"" + String(sysConfig.schedule[i].hour) + "\"" + disabledAttr + "> : "
      + "<input type=\"number\" name=\"m" + String(i) + "\" min=\"0\" max=\"59\" value=\"" + String(sysConfig.schedule[i].minute) + "\"" + disabledAttr + ">"
      + toggleSwitch("a" + String(i), sysConfig.schedule[i].active, disabledAttr, "margin-left:8px")
      + "</div></div>");
  }
  client.println("</div>");
}

static void renderSettingsCard(BufferedClient& client, bool scheduleLocked) {
  String disabledAttr = scheduleLocked ? " disabled" : "";
  client.println("<div class=\"card\"><h2>Settings</h2>");
  client.println("<div class=\"row\"><span>System Enabled</span>" + toggleSwitch("en", sysConfig.globalEnabled, disabledAttr) + "</div>");
  client.println("</div>");
}

static void renderPumpCard(BufferedClient& client, bool scheduleLocked) {
  String disabledAttr = scheduleLocked ? " disabled" : "";
  client.println("<div class=\"card\"><h2>Pump Settings</h2>");
  client.println("<div class=\"row\"><span>Enabled</span>" + toggleSwitch("pen", sysConfig.pumpEnabled, disabledAttr) + "</div>"
    + "<div class=\"row\"><span>Duration (sec)</span><input type=\"number\" name=\"dur\" value=\"" + String(sysConfig.runDuration) + "\"" + disabledAttr + "></div>");
  client.println("</div>");
}

static void renderLightCard(BufferedClient& client, bool scheduleLocked) {
  String disabledAttr = scheduleLocked ? " disabled" : "";
  client.println("<div class=\"card\"><h2>Light Settings</h2>");
  client.println("<div class=\"row\"><span>Enabled</span>" + toggleSwitch("len", sysConfig.lightEnabled, disabledAttr) + "</div>"
    + "<div class=\"row\"><span>Lead Time (minutes)</span><input type=\"number\" name=\"lead\" value=\"" + String(sysConfig.lightLeadMinutes) + "\"" + disabledAttr + "></div>");
  client.println("</div>");
}

static void renderSleepCard(BufferedClient& client, const String& hp) {
  float opts[] = {10.5, 9.0, 7.5, 6.0};

  client.println("<div class=\"card\"><h2>Sleep Cycles</h2>"
    + String("<form action=\"/SET_SLEEP_DELAY\" method=\"GET\">") + hp
    + "<div class=\"row\" style=\"border-bottom:1px solid #eee\"><span>Time to fall asleep (min)</span>"
    + "<div style=\"display:flex;gap:5px\"><input type=\"number\" name=\"sld\" value=\"" + String(sysConfig.fallingAsleepMinutes) + "\" min=\"0\">"
    + "<button type=\"submit\" style=\"padding:8px; border-radius:6px; border:none; background:#007aff; color:white\">OK</button>"
    + "</div></div></form>"
    + "<h3>If you go to sleep now</h3><div class=\"cycle-grid\">");
  for (int i = 0; i < 4; i++) {
    client.println("<div class=\"cycle-item\">" + String(opts[i], 1) + "h (" + String((int)(opts[i] / 1.5)) + " cycles)"
      + "<span class=\"cycle-time\">" + getWakeTime(opts[i]) + "</span></div>");
  }
  client.println("</div><h3>If you want to wake up at...</h3>"
    + String("<form action=\"/CALC_BED\" method=\"GET\">") + hp
    + "<div class=\"calc-row\">"
    + "<input type=\"number\" name=\"wh\" value=\"" + String(targetWakeH) + "\" min=\"0\" max=\"23\"> : "
    + "<input type=\"number\" name=\"wm\" value=\"" + String(targetWakeM) + "\" min=\"0\" max=\"59\">"
    + "<button type=\"submit\" style=\"padding:10px 15px; border-radius:6px; border:none; background:#007aff; color:white; font-weight:bold\">CALCULATE</button>"
    + "</div></form>");
  if (showBedTimes) {
    client.println("<div class=\"cycle-grid\">");
    for (int i = 0; i < 4; i++) {
      client.println("<div class=\"cycle-item\">Sleep " + String(opts[i], 1) + "h"
        + "<span class=\"cycle-time\">" + getBedTime(targetWakeH, targetWakeM, opts[i]) + "</span></div>");
    }
    client.println("</div>");
  }
  client.println("</div>");
}

static void renderBlindCard(BufferedClient& client, const String& pinParam) {
  String stateLabel, stateColor;
  if (blindManualActive && blindManualDirection == 1) {
    stateLabel = "OPENING"; stateColor = "#34c759";
  } else if (blindManualActive && blindManualDirection == -1) {
    stateLabel = "CLOSING"; stateColor = "#ff9500";
  } else {
    stateLabel = "IDLE"; stateColor = "#8e8e93";
  }

  int pos = currentBlindPosition();
  String posStr = (pos < 0) ? "unknown" : (String(pos) + "% open");

  client.println("<div class=\"card\"><h2>Blind</h2>");
  client.println("<p style=\"font-weight:bold;color:" + stateColor + ";margin-bottom:2px\">" + stateLabel + "</p>");
  client.println("<p style=\"margin:0 0 10px;color:#555\">Position: <b>" + posStr + "</b></p>");
  client.println("<div style=\"display:flex;gap:8px\">");
  client.println("<a href=\"/BLIND_OPEN?" + pinParam + "\" style=\"flex:1\"><button class=\"btn\" style=\"background:#34c759;color:white;margin:0\">OPEN</button></a>");
  client.println("<a href=\"/BLIND_CLOSE?" + pinParam + "\" style=\"flex:1\"><button class=\"btn\" style=\"background:#ff9500;color:white;margin:0\">CLOSE</button></a>");
  client.println("<a href=\"/BLIND_STOP?" + pinParam + "\" style=\"flex:1\"><button class=\"btn btn-stop\" style=\"margin:0\">STOP</button></a>");
  client.println("</div>");
  client.println("<div style=\"margin-top:14px;padding-top:12px;border-top:1px solid #f0f0f0\">");
  client.println("<p style=\"color:#bbb;font-size:0.75rem;margin:0 0 8px\">Position override (motor does not move)</p>");
  client.println("<div style=\"display:flex;gap:8px\">");
  client.println("<a href=\"/BLIND_FORCE_POS?" + pinParam + "&pos=0\" onclick=\"return confirm('Force position to CLOSED (0% open)? The motor will not move.')\" style=\"flex:1\">");
  client.println("<button class=\"btn\" style=\"background:#ff3b30;color:white;margin:0;font-size:0.88rem;padding:10px\">Force Closed</button></a>");
  client.println("<a href=\"/BLIND_FORCE_POS?" + pinParam + "&pos=100\" onclick=\"return confirm('Force position to OPEN (100% open)? The motor will not move.')\" style=\"flex:1\">");
  client.println("<button class=\"btn\" style=\"background:#34c759;color:white;margin:0;font-size:0.88rem;padding:10px\">Force Open</button></a>");
  client.println("</div></div></div>");
}

static void renderBlindSettingsCard(BufferedClient& client, bool scheduleLocked) {
  String disabledAttr = scheduleLocked ? " disabled" : "";
  client.println("<div class=\"card\"><h2>Blind Settings</h2>");
  client.println("<div class=\"row\"><span>Enabled</span>" + toggleSwitch("ben", sysConfig.blindEnabled, disabledAttr) + "</div>");
  client.println("<div class=\"row\"><span>Open Lead Time (min)</span><input type=\"number\" name=\"blead\" value=\"" + String(sysConfig.blindLeadMinutes) + "\" min=\"0\"" + disabledAttr + "></div>");
  client.println("<div class=\"row\"><span>Open Duration (sec)</span><input type=\"number\" name=\"bopen\" value=\"" + String(sysConfig.blindOpenDuration) + "\" min=\"1\"" + disabledAttr + "></div>");
  client.println("<div class=\"row\"><span>Close Duration (sec)</span><input type=\"number\" name=\"bclose\" value=\"" + String(sysConfig.blindCloseDuration) + "\" min=\"1\"" + disabledAttr + "></div>");
  client.println("<h3>End-of-run Slowdown (last 30%)</h3>");
  client.println("<p style=\"font-size:0.75rem;color:#999;text-align:left;margin:2px 0 10px\">First 70% always at full power &mdash; applies to scheduled runs</p>");
  const char* pctLabels[6] = {"70%", "75%", "80%", "85%", "90%", "95%"};
  for (int i = 0; i < 6; i++) {
    String val = String(sysConfig.motorSlowdown[i]);
    String iStr = String(i);
    client.println(
      "<div class=\"prow\">"
      "<span class=\"plbl\">" + String(pctLabels[i]) + "</span>"
      + "<input type=\"range\" name=\"p" + iStr + "\" min=\"0\" max=\"100\" value=\"" + val + "\""
      + disabledAttr
      + " oninput=\"document.getElementById('pv" + iStr + "').textContent=this.value+'%'\">"
      + "<span class=\"pval\" id=\"pv" + iStr + "\">" + val + "%</span>"
      + "</div>"
    );
  }
  client.println("<p style=\"font-size:0.72rem;color:#bbb;text-align:right;margin:4px 0 0\">At 100%: motor stops</p>");
  client.println("</div>");
}

static void renderOneShotCard(BufferedClient& client, bool scheduleLocked, const String& pinParam, const String& hp) {
  client.println("<div class=\"card\"><h2>One-Shot Alarm</h2>");
  if (oneShot.armed) {
    unsigned long nowEpoch = timeClient.getEpochTime();
    long secsLeft = (long)oneShot.triggerEpoch - (long)nowEpoch;
    if (secsLeft < 0) secsLeft = 0;
    int hh = (int)(secsLeft / 3600);
    int mm = (int)((secsLeft % 3600) / 60);
    int ss = (int)(secsLeft % 60);

    long curTotalSec = (long)timeClient.getHours() * 3600L + (long)timeClient.getMinutes() * 60L + (long)timeClient.getSeconds();
    long triggerTotalSec = ((curTotalSec + secsLeft) % 86400L + 86400L) % 86400L;
    int trgH = (int)(triggerTotalSec / 3600L);
    int trgM = (int)((triggerTotalSec % 3600L) / 60L);

    client.println("<p style=\"margin:0 0 10px\">Trigger at <b>" + formatTime(trgH, trgM) + "</b></p>");
    client.println("<p id=\"osCountdown\" style=\"font-size:1.4rem;font-weight:bold;color:#007aff;margin:0 0 15px\">"
      + String(hh) + "h " + String(mm) + "m " + String(ss) + "s</p>");
    client.println("<div class=\"row\"><span>Pump</span><b>" + String(oneShot.pumpEnabled ? "ON" : "OFF") + "</b></div>");
    client.println("<div class=\"row\"><span>Light</span><b>"
      + (oneShot.lightEnabled ? ("ON (lead " + String(oneShot.lightLeadMinutes) + " min)") : String("OFF")) + "</b></div>");
    client.println("<div class=\"row\"><span>Blind</span><b>"
      + (oneShot.blindEnabled ? ("ON (lead " + String(oneShot.blindLeadMinutes) + " min)") : String("OFF")) + "</b></div>");
    client.println("<a href=\"/CANCEL_ONESHOT?" + pinParam + "\" onclick=\"return confirm('Cancel the one-shot alarm?')\">"
      + "<button class=\"btn btn-stop\">Cancel</button></a>");
    client.println("<script>(function(){var s=" + String(secsLeft) + ";var el=document.getElementById('osCountdown');"
      "setInterval(function(){if(s<=0)return;s--;var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;"
      "el.textContent=h+'h '+m+'m '+sec+'s';},1000);})();</script>");
  } else {
    String disabledAttr = scheduleLocked ? " disabled" : "";
    client.println("<form action=\"/ARM_ONESHOT\" method=\"GET\">" + hp);
    client.println(String("<div class=\"row\"><span>Trigger in</span><div>")
      + "<input type=\"number\" name=\"osH\" min=\"0\" max=\"23\" value=\"0\"" + disabledAttr + "> h "
      + "<input type=\"number\" name=\"osM\" min=\"0\" max=\"59\" value=\"25\"" + disabledAttr + "> min</div></div>");
    client.println("<div class=\"row\"><span>Pump</span>" + toggleSwitch("ospen", oneShot.pumpEnabled, disabledAttr) + "</div>");
    client.println("<div class=\"row\"><span>Light</span>" + toggleSwitch("oslen", oneShot.lightEnabled, disabledAttr) + "</div>");
    client.println("<div class=\"row\"><span>Light Lead Time (min)</span><input type=\"number\" name=\"oslead\" value=\"" + String(oneShot.lightLeadMinutes) + "\" min=\"0\"" + disabledAttr + "></div>");
    client.println("<div class=\"row\"><span>Blind</span>" + toggleSwitch("osben", oneShot.blindEnabled, disabledAttr) + "</div>");
    client.println("<div class=\"row\"><span>Blind Lead Time (min)</span><input type=\"number\" name=\"osblead\" value=\"" + String(oneShot.blindLeadMinutes) + "\" min=\"0\"" + disabledAttr + "></div>");
    lockedButton(client, "ARM ONE-SHOT ALARM", scheduleLocked);
    client.println("</form>");
  }
  client.println("</div>");
}

static void renderFirmwareCard(BufferedClient& client, const String& pinParam, bool scheduleLocked) {
  client.println("<div class=\"card\"><h2>Firmware</h2>");
  client.println("<div class=\"row\"><span>Current version</span><b>" + String(FIRMWARE_VERSION) + "</b></div>");

  if (otaState == OTA_ERROR) {
    client.println("<div class=\"error-msg\" style=\"margin:15px 0\">" + otaErrorMsg + "</div>");
    client.println("<a href=\"/OTA_DISMISS?" + pinParam + "\"><button class=\"btn btn-stop\">Dismiss</button></a>");
  } else if (otaUpdateAvailable) {
    client.println("<div class=\"row\"><span>Latest version</span><b style=\"color:#34c759\">" + otaLatestVersion + "</b></div>");
    if (scheduleLocked || blindManualActive) {
      client.println("<button type=\"button\" class=\"btn\" style=\"background:#ccc;cursor:not-allowed\" disabled>UPDATE NOW</button>");
    } else {
      client.println("<a href=\"/OTA_APPLY?" + pinParam + "\" onclick=\"return confirm('Update firmware now? The device will reboot and may be unreachable for a few minutes. The page returns to the dashboard automatically.')\">");
      client.println("<button class=\"btn btn-manual\">UPDATE NOW</button></a>");
    }
  } else {
    client.println("<p style=\"color:#8e8e93;font-size:0.85rem;margin:10px 0 0\">Firmware up to date</p>");
  }

  if (otaCheckNote.length() > 0) {
    client.println("<p style=\"color:#ff9500;font-size:0.85rem;font-weight:bold;margin:10px 0 0\">" + otaCheckNote + "</p>");
  }
  if (otaLastCheckEpoch > 0) {
    unsigned long secOfDay = otaLastCheckEpoch % 86400UL;
    client.println("<p style=\"color:#8e8e93;font-size:0.75rem;margin:6px 0 0\">Last check: "
      + formatTime((int)(secOfDay / 3600UL), (int)((secOfDay % 3600UL) / 60UL)) + "</p>");
  }

  client.println("<a href=\"/OTA_CHECK?" + pinParam + "\"><button class=\"btn btn-stop\" style=\"margin-top:8px\">Check for updates</button></a>");
  client.println("</div>");
}

static void renderStatusCard(BufferedClient& client, const String& pinParam) {
  client.print("<div class=\"card\"><h2>Status</h2>"
    + String("<p>") + daysOfWeek[timeClient.getDay()] + ", " + timeClient.getFormattedTime() + "</p>"
    + "<p style=\"font-weight:bold\">"
    + String(manualOverride ? "<span style='color:#ff3b30'>MANUAL ON</span>" : (digitalRead(PUMP_PIN) ? "RUNNING" : "IDLE"))
    + "</p>");
  client.print("<a href=\"/TOGGLE?" + pinParam + "\" " + String(manualOverride ? "" : "onclick=\"return confirm('Confirm manual pump start?')\"") + ">"
    + "<button class=\"btn " + String(manualOverride ? "btn-stop" : "btn-manual") + "\">"
    + String(manualOverride ? "STOP PUMP" : "MANUAL START")
    + "</button></a></div></body></html>");
}

static void renderDashboard(BufferedClient& client, const String& hp, const String& pinParam, bool scheduleLocked) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println("Cache-Control: no-store");
  client.println(authCookieHeader());
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><link rel=\"icon\" href=\"data:,\">");
  client.println("<style>");
  client.println("body{font-family:-apple-system,system-ui,sans-serif;text-align:center;margin:0;padding:10px;background-color:#efeff4;color:#333;}.card{background:white;padding:15px;margin:15px auto;max-width:500px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.05);}h2{font-size:1.1rem;margin-top:0;color:#666;border-bottom:1px solid #eee;padding-bottom:8px;text-transform:uppercase;}h3{font-size:0.9rem;color:#888;margin:15px 0 5px 0;text-align:left;border-left:3px solid #007aff;padding-left:8px;}.row{display:flex;align-items:center;justify-content:space-between;padding:10px 0;border-bottom:1px solid #f0f0f0;}input[type=number]{padding:8px;width:50px;text-align:center;font-size:16px;border:1px solid #ccc;border-radius:6px;}.btn{width:100%;padding:15px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;margin-top:10px;}.btn-save{background-color:#007aff;color:white;}.btn-manual{background-color:#ff3b30;color:white;}.btn-stop{background-color:#8e8e93;color:white;}.cycle-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px;}.cycle-item{background:#f8f8f8;padding:10px;border-radius:8px;font-size:0.85rem;}.cycle-time{font-weight:bold;color:#007aff;display:block;font-size:1.1rem;}.calc-row{display:flex;align-items:center;justify-content:flex-start;gap:5px;padding:10px 0;}.error-msg{background:#ff3b30;color:white;padding:12px;border-radius:8px;margin:15px auto;max-width:500px;font-weight:bold;}.prow{display:flex;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid #f5f5f5;}.plbl{width:36px;font-size:0.82rem;color:#666;text-align:right;flex-shrink:0;}.prow input[type=range]{flex:1;min-width:0;height:28px;cursor:pointer;}.pval{width:38px;font-size:0.9rem;font-weight:bold;color:#007aff;text-align:right;flex-shrink:0;}.switch{position:relative;display:inline-block;width:46px;height:26px;flex-shrink:0;}.switch input{opacity:0;width:0;height:0;}.switch .slider{position:absolute;top:0;left:0;right:0;bottom:0;background-color:#ccc;transition:.2s;border-radius:26px;cursor:pointer;}.switch .slider:before{position:absolute;content:\"\";height:20px;width:20px;left:3px;bottom:3px;background-color:#fff;transition:.2s;border-radius:50%;}.switch input:checked+.slider{background-color:#34c759;}.switch input:checked+.slider:before{transform:translateX(20px);}.switch input:disabled+.slider{opacity:.5;cursor:not-allowed;}");
  client.println("</style>");

  client.println("<script>");
  client.println("var currentLockState = " + String(scheduleLocked ? "true" : "false") + ";");
  client.println("setInterval(function() {");
  client.println("  fetch('/CHECK_LOCK').then(r => r.text()).then(state => {");
  client.println("    var newLockState = (state === '1');");
  client.println("    if (newLockState !== currentLockState) location.reload();");
  client.println("  });");
  client.println("}, 5000);");
  client.println("</script>");

  client.println("</head><body>");

  if (scheduleLocked && scheduleErrorMsg.length() > 0) {
    client.println("<div class=\"error-msg\">" + scheduleErrorMsg + "</div>");
  }

  client.println("<form action=\"/SAVE_ALL\" method=\"GET\">" + hp);
  renderScheduleCard(client, scheduleLocked);
  renderSettingsCard(client, scheduleLocked);
  renderPumpCard(client, scheduleLocked);
  renderLightCard(client, scheduleLocked);
  renderBlindSettingsCard(client, scheduleLocked);
  client.println("<div class=\"card\" style=\"padding:10px\">");
  lockedButton(client, "SAVE ALL SETTINGS", scheduleLocked);
  client.println("</div></form>");
  renderOneShotCard(client, scheduleLocked, pinParam, hp);
  renderSleepCard(client, hp);
  renderBlindCard(client, pinParam);
  renderFirmwareCard(client, pinParam, scheduleLocked);
  renderStatusCard(client, pinParam);
}

void handleWebRequest() {
  WiFiClient client = server.available();
  if (!client) return;
  lastWebActivityMs = millis();

  String currentLine = "";
  String request = "";
  unsigned long requestStartMs = millis();

  while (client.connected()) {
    unsigned long elapsed = millis() - requestStartMs;
    if (elapsed > HTTP_REQUEST_TIMEOUT_MS) break;
    if (request.length() == 0 && elapsed > HTTP_FIRSTBYTE_TIMEOUT_MS) break;
    if (client.available()) {
      char c = client.read();
      request += c;
      if (c == '\n') {
        if (currentLine.length() == 0) {
          BufferedClient out(client);

          int reqLineEnd = request.indexOf('\n');
          String requestLine = (reqLineEnd > 0) ? request.substring(0, reqLineEnd) : request;

          if (requestLine.indexOf("GET /favicon") >= 0 || requestLine.indexOf("GET /apple-touch-icon") >= 0) {
            out.println("HTTP/1.1 404 Not Found");
            out.println("Connection: close");
            out.println();
            out.flush();
            break;
          }

          String urlPin = extractPinValue(requestLine, "pin=");
          String cookiePin = "";
          int cookieIdx = request.indexOf("Cookie:");
          if (cookieIdx >= 0) {
            int cookieEnd = request.indexOf('\n', cookieIdx);
            String cookieLine = (cookieEnd > 0) ? request.substring(cookieIdx, cookieEnd) : request.substring(cookieIdx);
            cookiePin = extractPinValue(cookieLine, "pin=");
          }
          bool isPinValid = (urlPin.length() > 0 && urlPin == String(access_pin))
            || (cookiePin.length() > 0 && cookiePin == String(access_pin));

          if (request.indexOf("GET /LOGIN") >= 0) {
            if (isPinValid) {
              out.println("HTTP/1.1 302 Found");
              out.println("Location: /?pin=" + String(access_pin));
              out.println(authCookieHeader());
              out.println("Connection: close");
              out.println();
            } else {
              servePinPage(out);
            }
            out.flush();
            break;
          }

          if (!isPinValid && request.indexOf("GET /CHECK_LOCK") < 0) {
            bool hasUrlPin = (urlPin.length() > 0);
            bool hasStaleCookie = (cookiePin.length() > 0);

            if (pinFailCount > 0 && (millis() - pinFirstFailTime) >= PIN_LOCKOUT_MS) {
              pinFailCount = 0;
            }

            if (pinFailCount >= PIN_MAX_ATTEMPTS) {
              int minsLeft = (int)((PIN_LOCKOUT_MS - (millis() - pinFirstFailTime)) / 60000) + 1;
              servePinPage(out, "Too many attempts. Try again in " + String(minsLeft) + " min.", true, hasStaleCookie);
            } else if (hasUrlPin) {
              if (pinFailCount == 0) pinFirstFailTime = millis();
              pinFailCount++;
              int remaining = PIN_MAX_ATTEMPTS - pinFailCount;
              if (remaining > 0) {
                servePinPage(out, "Incorrect PIN. Remaining attempts: " + String(remaining), false, hasStaleCookie);
              } else {
                servePinPage(out, "Too many attempts. Try again in 5 min.", true, hasStaleCookie);
              }
            } else {
              servePinPage(out, "", false, hasStaleCookie);
            }
            out.flush();
            break;
          }

          pinFailCount = 0;

          String pinParam = "pin=" + String(access_pin);
          String hp = "<input type=\"hidden\" name=\"pin\" value=\"" + String(access_pin) + "\">";

          if (request.indexOf("GET /CHECK_LOCK") >= 0) {
            out.println("HTTP/1.1 200 OK");
            out.println("Content-type:text/plain");
            out.println("Connection: close");
            out.println();
            out.print(isScheduleLocked() ? "1" : "0");
            out.flush();
            break;
          }

          if (request.indexOf("GET /OTA_APPLY") >= 0) {
            if (isScheduleLocked() || blindManualActive) {
              // Redirect instead of rendering the dashboard as the response to
              // this URL — otherwise the browser's address bar stays on
              // "/OTA_APPLY?..." and a later reload would re-attempt the apply.
              out.println("HTTP/1.1 302 Found");
              out.println("Location: /?" + pinParam);
              out.println("Connection: close");
              out.println();
              out.flush();
              break;
            }
            // Send a self-reloading "updating" page, close this socket, then free
            // the ESP32's other sockets (the :80 listener and the NTP UDP socket)
            // before starting the OTA. The OTAUpdate examples run the OTA in a clean
            // network state right after WiFi connect; with our web server and NTP
            // sockets still open, the apply step (which resets the modem) hangs.
            // Closing the browser socket first also gives it a real response instead
            // of retrying /OTA_APPLY (which caused a reboot loop). On success
            // startOtaUpdate() reboots and never returns; on failure we restore the
            // services so the dashboard keeps working (the OTA_ERROR state is then
            // shown when this page reloads).
            out.println("HTTP/1.1 200 OK");
            out.println("Content-type:text/html");
            out.println("Connection: close");
            out.println();
            out.println("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
              "<meta http-equiv=\"refresh\" content=\"180;url=/?" + pinParam + "\">"
              "<title>Updating...</title></head>"
              "<body style=\"font-family:sans-serif;text-align:center;padding-top:40px\">"
              "<h2>Firmware update in progress...</h2>"
              "<p>The device is downloading and applying the new firmware, then it will reboot.</p>"
              "<p>This page returns to the dashboard automatically as soon as the device is back online.</p>"
              "<script>function n(){setTimeout(p,4000);}"
              "function p(){fetch('/CHECK_LOCK',{cache:'no-store'}).then(function(r){"
              "if(r.ok){location.replace('/?" + pinParam + "');}else{n();}}).catch(n);}"
              "setTimeout(p,20000);</script>"
              "</body></html>");
            out.flush();
            client.flush();
            client.stop();
            server.end();     // free the :80 listening socket
            ntpUDP.stop();    // free the NTP UDP socket
            delay(100);       // let the socket teardown settle before the OTA
            if (!startOtaUpdate()) {
              // OTA failed (success reboots the device) — restore network services.
              server.begin();
              timeClient.begin();
            }
            return;
          }

          bool actionHandled = handleRoutes(request);
          if (actionHandled) {
            // Redirect to the clean dashboard URL so the browser's address bar
            // (and any later reload, e.g. the dashboard's /CHECK_LOCK polling)
            // never points at an action URL with stale query params — see the
            // comment on handleRoutes() for why that matters.
            out.println("HTTP/1.1 302 Found");
            out.println("Location: /?" + pinParam);
            out.println("Connection: close");
            out.println();
          } else {
            // Force a fresh OTA check on a real dashboard load while there's
            // OTA state worth re-verifying (a pending update or a failed apply) —
            // otherwise the card stays stuck showing "UPDATE NOW"/the old error
            // until the next periodic check (up to OTA_CHECK_INTERVAL) or a manual
            // "Check for updates" tap. Skipped when everything's already idle, and
            // throttled to at most once per OTA_DASHBOARD_RECHECK_MS, so page
            // loads don't pay a blocking HTTPS round-trip every time.
            if ((otaState == OTA_ERROR || otaUpdateAvailable)
                && (millis() - lastOtaCheck >= OTA_DASHBOARD_RECHECK_MS)
                && !otaCheckBusy()) {
              checkForUpdateNow(1);
            }
            bool scheduleLocked = isScheduleLocked();
            renderDashboard(out, hp, pinParam, scheduleLocked);
          }
          out.flush();
          break;

        } else {
          currentLine = "";
        }
      } else if (c != '\r') {
        currentLine += c;
      }
    }
  }
  client.stop();
}
