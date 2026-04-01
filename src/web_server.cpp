#include "web_server.h"
#include "globals.h"
#include "scheduler.h"
#include "time_utils.h"
#include "storage.h"
#include <Arduino.h>

// ─── Rate limiting ────────────────────────────────────────────────────────────

static int pinFailCount = 0;
static unsigned long pinFirstFailTime = 0;
static const int PIN_MAX_ATTEMPTS = 5;
static const unsigned long PIN_LOCKOUT_MS = 300000UL; // 5 minutes

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Extracts value of "key" from a query string.
// Reads until 'terminator' or space. Returns "" if key not found.
static String parseParam(const String& req, const String& key, char terminator = '&') {
  int idx = req.indexOf(key);
  if (idx < 0) return "";
  int start = idx + key.length();
  int end = req.indexOf(terminator, start);
  if (end < 0) end = req.indexOf(' ', start);
  if (end < 0) return req.substring(start);
  return req.substring(start, end);
}

// Renders a form submit button, disabled (grey) when schedule is locked.
static void lockedButton(WiFiClient& client, const String& label, bool scheduleLocked) {
  if (scheduleLocked) {
    client.println("<button type=\"button\" class=\"btn\" style=\"background:#ccc;cursor:not-allowed\" disabled>" + label + "</button>");
  } else {
    client.println("<button type=\"submit\" class=\"btn btn-save\">" + label + "</button>");
  }
}

// ─── PIN page ─────────────────────────────────────────────────────────────────

static void servePinPage(WiFiClient& client, const String& errorMsg = "", bool locked = false) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  client.println("<style>body{font-family:-apple-system,system-ui,sans-serif;text-align:center;margin:0;padding:40px 10px;background-color:#efeff4;}.card{background:white;padding:30px;margin:60px auto;max-width:300px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.05);}h2{font-size:1.2rem;color:#333;margin-bottom:20px;}#pinInput{padding:12px;width:100%;text-align:center;font-size:26px;letter-spacing:10px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;margin-bottom:15px;caret-color:transparent;}.error{background:#ff3b30;color:white;padding:10px;border-radius:8px;margin-bottom:15px;font-size:0.9rem;}</style>");
  client.println("</head><body><div class=\"card\"><h2>ClockHub</h2>");
  if (errorMsg.length() > 0) {
    client.println("<div class=\"error\">" + errorMsg + "</div>");
  } else {
    client.println("<p style=\"color:#888;font-size:0.9rem;margin-bottom:15px\">Inserisci il PIN per accedere</p>");
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

// ─── Route handlers (side-effects only, no response) ─────────────────────────

static void handleRoutes(const String& request) {
  if (request.indexOf("GET /TOGGLE") >= 0) {
    manualOverride = !manualOverride;
    if (!manualOverride) digitalWrite(PUMP_PIN, LOW);
  }
  else if (request.indexOf("GET /SET_GLOBAL") >= 0) {
    if (!isScheduleLocked()) {
      String durVal = parseParam(request, "dur=");
      if (durVal.length() > 0) sysConfig.runDuration = durVal.toInt();
      sysConfig.globalEnabled = (request.indexOf("en=") > 0);
      saveConfig();
      scheduleErrorMsg = "";
    }
  }
  else if (request.indexOf("GET /SET_LIGHT_CONFIG") >= 0) {
    if (!isScheduleLocked()) {
      String leadVal = parseParam(request, "lead=", ' ');
      if (leadVal.length() > 0) sysConfig.lightLeadMinutes = leadVal.toInt();
      saveConfig();
      scheduleErrorMsg = "";
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
  else if (request.indexOf("GET /SET_SCHEDULE") >= 0) {
    if (!isScheduleLocked()) {
      for (int i = 0; i < 7; i++) {
        String hK = "h" + String(i) + "=";
        String mK = "m" + String(i) + "=";
        String aK = "a" + String(i) + "=";
        String hVal = parseParam(request, hK);
        String mVal = parseParam(request, mK);
        if (hVal.length() > 0) sysConfig.schedule[i].hour   = hVal.toInt();
        if (mVal.length() > 0) sysConfig.schedule[i].minute = mVal.toInt();
        sysConfig.schedule[i].active = (request.indexOf(aK) > 0);
      }
      saveConfig();
      scheduleErrorMsg = "";
    }
  }
}

// ─── Dashboard card renderers ─────────────────────────────────────────────────

static void renderScheduleCard(WiFiClient& client, const String& hp, bool scheduleLocked) {
  String disabledAttr = scheduleLocked ? " disabled" : "";
  client.println("<div class=\"card\"><h2>Weekly Schedule</h2>");
  client.println("<form action=\"/SET_SCHEDULE\" method=\"GET\">" + hp);
  for (int i = 0; i < 7; i++) {
    client.println("<div class=\"row\"><b>" + String(daysOfWeek[i]) + "</b><div>"
      + "<input type=\"number\" name=\"h" + String(i) + "\" min=\"0\" max=\"23\" value=\"" + String(sysConfig.schedule[i].hour) + "\"" + disabledAttr + "> : "
      + "<input type=\"number\" name=\"m" + String(i) + "\" min=\"0\" max=\"59\" value=\"" + String(sysConfig.schedule[i].minute) + "\"" + disabledAttr + ">"
      + "<input type=\"checkbox\" name=\"a" + String(i) + "\" " + (sysConfig.schedule[i].active ? "checked" : "") + " style=\"width:20px;height:20px;margin-left:8px\"" + disabledAttr + ">"
      + "</div></div>");
  }
  lockedButton(client, "SAVE SCHEDULE", scheduleLocked);
  client.println("</form></div>");
}

static void renderSettingsCard(WiFiClient& client, const String& hp, bool scheduleLocked) {
  String disabledAttr = scheduleLocked ? " disabled" : "";
  client.println("<div class=\"card\"><h2>Settings</h2>");
  client.println("<form action=\"/SET_GLOBAL\" method=\"GET\">" + hp);
  client.println("<div class=\"row\"><span>Pump Duration (sec)</span><input type=\"number\" name=\"dur\" value=\"" + String(sysConfig.runDuration) + "\"" + disabledAttr + "></div>"
    + "<div class=\"row\"><span>System Enabled</span><input type=\"checkbox\" name=\"en\" " + String(sysConfig.globalEnabled ? "checked" : "") + " style=\"width:20px;height:20px\"" + disabledAttr + "></div>");
  lockedButton(client, "UPDATE SETTINGS", scheduleLocked);
  client.println("</form></div>");
}

static void renderLightCard(WiFiClient& client, const String& hp, bool scheduleLocked) {
  String disabledAttr = scheduleLocked ? " disabled" : "";
  client.println("<div class=\"card\"><h2>Light Settings</h2>");
  client.println("<form action=\"/SET_LIGHT_CONFIG\" method=\"GET\">" + hp
    + "<div class=\"row\"><span>Lead Time (minutes)</span><input type=\"number\" name=\"lead\" value=\"" + String(sysConfig.lightLeadMinutes) + "\"" + disabledAttr + "></div>");
  lockedButton(client, "UPDATE LIGHT CONFIG", scheduleLocked);
  client.println("</form></div>");
}

static void renderSleepCard(WiFiClient& client, const String& hp) {
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

static void renderStatusCard(WiFiClient& client, const String& pinParam) {
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

// ─── Dashboard ────────────────────────────────────────────────────────────────

static void renderDashboard(WiFiClient& client, const String& hp, const String& pinParam, bool scheduleLocked) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  client.println("<style>");
  client.println("body{font-family:-apple-system,system-ui,sans-serif;text-align:center;margin:0;padding:10px;background-color:#efeff4;color:#333;}.card{background:white;padding:15px;margin:15px auto;max-width:500px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.05);}h2{font-size:1.1rem;margin-top:0;color:#666;border-bottom:1px solid #eee;padding-bottom:8px;text-transform:uppercase;}h3{font-size:0.9rem;color:#888;margin:15px 0 5px 0;text-align:left;border-left:3px solid #007aff;padding-left:8px;}.row{display:flex;align-items:center;justify-content:space-between;padding:10px 0;border-bottom:1px solid #f0f0f0;}input[type=number]{padding:8px;width:50px;text-align:center;font-size:16px;border:1px solid #ccc;border-radius:6px;}.btn{width:100%;padding:15px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;margin-top:10px;}.btn-save{background-color:#007aff;color:white;}.btn-manual{background-color:#ff3b30;color:white;}.btn-stop{background-color:#8e8e93;color:white;}.cycle-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px;}.cycle-item{background:#f8f8f8;padding:10px;border-radius:8px;font-size:0.85rem;}.cycle-time{font-weight:bold;color:#007aff;display:block;font-size:1.1rem;}.calc-row{display:flex;align-items:center;justify-content:flex-start;gap:5px;padding:10px 0;}.error-msg{background:#ff3b30;color:white;padding:12px;border-radius:8px;margin:15px auto;max-width:500px;font-weight:bold;}");
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

  renderScheduleCard(client, hp, scheduleLocked);
  renderSettingsCard(client, hp, scheduleLocked);
  renderLightCard(client, hp, scheduleLocked);
  renderSleepCard(client, hp);
  renderStatusCard(client, pinParam);
}

// ─── Public entry point ───────────────────────────────────────────────────────

void handleWebRequest() {
  WiFiClient client = server.available();
  if (!client) return;

  String currentLine = "";
  String request = "";

  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      request += c;
      if (c == '\n') {
        if (currentLine.length() == 0) {

          bool isPinValid = (request.indexOf("pin=" + String(access_pin)) >= 0);

          if (request.indexOf("GET /LOGIN") >= 0) {
            if (isPinValid) {
              client.println("HTTP/1.1 302 Found");
              client.println("Location: /?pin=" + String(access_pin));
              client.println("Connection: close");
              client.println();
            } else {
              servePinPage(client);
            }
            break;
          }

          if (!isPinValid && request.indexOf("GET /CHECK_LOCK") < 0) {
            bool hasPin = (request.indexOf("pin=") >= 0);

            // Reset lockout after 5 minutes
            if (pinFailCount > 0 && (millis() - pinFirstFailTime) >= PIN_LOCKOUT_MS) {
              pinFailCount = 0;
            }

            if (pinFailCount >= PIN_MAX_ATTEMPTS) {
              int minsLeft = (int)((PIN_LOCKOUT_MS - (millis() - pinFirstFailTime)) / 60000) + 1;
              servePinPage(client, "Troppi tentativi. Riprova tra " + String(minsLeft) + " min.", true);
            } else if (hasPin) {
              if (pinFailCount == 0) pinFirstFailTime = millis();
              pinFailCount++;
              int remaining = PIN_MAX_ATTEMPTS - pinFailCount;
              if (remaining > 0) {
                servePinPage(client, "PIN non corretto. Tentativi rimanenti: " + String(remaining));
              } else {
                servePinPage(client, "Troppi tentativi. Riprova tra 5 min.", true);
              }
            } else {
              servePinPage(client);
            }
            break;
          }

          // Valid PIN — reset fail counter
          pinFailCount = 0;

          String pinParam = "pin=" + String(access_pin);
          String hp = "<input type=\"hidden\" name=\"pin\" value=\"" + String(access_pin) + "\">";

          if (request.indexOf("GET /CHECK_LOCK") >= 0) {
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/plain");
            client.println("Connection: close");
            client.println();
            client.print(isScheduleLocked() ? "1" : "0");
            break;
          }

          handleRoutes(request);
          bool scheduleLocked = isScheduleLocked();
          renderDashboard(client, hp, pinParam, scheduleLocked);
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
