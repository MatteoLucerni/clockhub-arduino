#include <WiFiS3.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include "config.h"

const char ssid[]       = WIFI_SSID;
const char pass[]       = WIFI_PASS;

const char* api_token   = API_TOKEN;
const char* monkey_id   = MONKEY_ID;
const char* duck_token  = DUCK_TOKEN;
const char* duck_domain = DUCK_DOMAIN;
const char* access_pin  = ACCESS_PIN;

const int PUMP_PIN = 7;
WiFiServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600);

unsigned long lastDuckDNSUpdate = 0;
const unsigned long DUCKDNS_INTERVAL = 300000;

struct DaySetting {
  bool active;
  int hour;
  int minute;
};

struct Config {
  bool globalEnabled;
  int runDuration;
  int lightLeadMinutes;
  int fallingAsleepMinutes;
  DaySetting schedule[7];
  int checkKey; 
};

Config sysConfig;
bool alarmTriggered = false;
bool lightTriggered = false;
bool manualOverride = false;
String scheduleErrorMsg = "";

int targetWakeH = 8;
int targetWakeM = 30;
bool showBedTimes = false;

const char* daysOfWeek[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

void updateDuckDNS() {
  WiFiSSLClient client;
  if (client.connect("www.duckdns.org", 443)) {
    String url = "/update?domains=" + String(duck_domain) + "&token=" + String(duck_token) + "&ip=";
    client.println("GET " + url + " HTTP/1.1");
    client.println("Host: www.duckdns.org");
    client.println("Connection: close");
    client.println();
    delay(500);
    client.stop();
  }
}

void triggerVoiceMonkey() {
  WiFiSSLClient client;
  if (client.connect("api-v2.voicemonkey.io", 443)) {
    String url = "/trigger?token=" + String(api_token) + "&device=" + String(monkey_id);
    client.println("GET " + url + " HTTP/1.1");
    client.println("Host: api-v2.voicemonkey.io");
    client.println("Connection: close");
    client.println();
    delay(500);
    client.stop();
  }
}

void loadConfig() {
  EEPROM.get(0, sysConfig);
  if (sysConfig.checkKey != 12345) {
    sysConfig.globalEnabled = true;
    sysConfig.runDuration = 10;
    sysConfig.lightLeadMinutes = 30;
    sysConfig.fallingAsleepMinutes = 15;
    sysConfig.checkKey = 12345;
    for (int i = 0; i < 7; i++) {
      sysConfig.schedule[i] = {false, 7, 30};
    }
    EEPROM.put(0, sysConfig);
  }
}

void saveConfig() {
  EEPROM.put(0, sysConfig);
}

String formatTime(int h, int m) {
  char buffer[6];
  sprintf(buffer, "%02d:%02d", h, m);
  return String(buffer);
}

String getWakeTime(float hoursFromNow) {
  int totalMinToAdd = (int)(hoursFromNow * 60) + sysConfig.fallingAsleepMinutes;
  int currentTotalMin = (timeClient.getHours() * 60) + timeClient.getMinutes();
  int resultMin = (currentTotalMin + totalMinToAdd) % 1440;
  return formatTime(resultMin / 60, resultMin % 60);
}

String getBedTime(int h, int m, float sleepHours) {
  int totalMinToSub = (int)(sleepHours * 60) + sysConfig.fallingAsleepMinutes;
  int targetTotalMin = (h * 60) + m;
  int resultMin = (targetTotalMin - totalMinToSub);
  while (resultMin < 0) resultMin += 1440;
  return formatTime(resultMin / 60, resultMin % 60);
}

bool isScheduleLocked() {
  if (!sysConfig.globalEnabled) return false;
  
  int curDay = timeClient.getDay();
  int curTotal = timeClient.getHours() * 60 + timeClient.getMinutes();
  
  for (int checkDay = 0; checkDay < 7; checkDay++) {
    if (!sysConfig.schedule[checkDay].active) continue;
    
    int almTotal = sysConfig.schedule[checkDay].hour * 60 + sysConfig.schedule[checkDay].minute;
    int minUntilAlarm;
    
    if (checkDay == curDay) {
      minUntilAlarm = almTotal - curTotal;
      if (minUntilAlarm < 0) minUntilAlarm += 1440;
    } else if (checkDay == ((curDay + 1) % 7)) {
      minUntilAlarm = (1440 - curTotal) + almTotal;
    } else {
      continue;
    }
    
    if (minUntilAlarm <= 60) {
      scheduleErrorMsg = "Some settings cannot be modified because an alarm is within 1 hour";
      return true;
    }
  }
  
  return false;
}

void servePinPage(WiFiClient client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  client.println("<style>body{font-family:-apple-system,system-ui,sans-serif;text-align:center;margin:0;padding:40px 10px;background-color:#efeff4;}.card{background:white;padding:30px;margin:60px auto;max-width:300px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.05);}h2{font-size:1.2rem;color:#333;}p{color:#888;font-size:0.9rem;}input[type=password]{padding:12px;width:100%;text-align:center;font-size:24px;letter-spacing:8px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;margin:15px 0;}.btn{width:100%;padding:15px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;background-color:#007aff;color:white;}</style>");
  client.println("</head><body><div class=\"card\"><h2>ClockHub</h2><p>Enter PIN to access</p>");
  client.println("<form action=\"/LOGIN\" method=\"GET\">");
  client.println("<input type=\"password\" name=\"pin\" maxlength=\"6\" placeholder=\"------\" autofocus>");
  client.println("<button type=\"submit\" class=\"btn\">ACCESS</button>");
  client.println("</form></div></body></html>");
}

void setup() {
  Serial.begin(115200);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  loadConfig();

  IPAddress arduinoIP(ARDUINO_IP_ARGS);
  IPAddress dns(DNS_IP_ARGS);
  IPAddress gateway(GATEWAY_IP_ARGS);
  IPAddress subnet(SUBNET_ARGS);
  WiFi.config(arduinoIP, dns, gateway, subnet);

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  server.begin();
  timeClient.begin();
  updateDuckDNS();
  lastDuckDNSUpdate = millis();
}

void loop() {
  timeClient.update();

  if (millis() - lastDuckDNSUpdate >= DUCKDNS_INTERVAL) {
    updateDuckDNS();
    lastDuckDNSUpdate = millis();
  }

  WiFiClient client = server.available();

  if (client) {
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
              servePinPage(client);
              break;
            }

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
            else if (request.indexOf("GET /TOGGLE") >= 0) {
              manualOverride = !manualOverride;
              if(!manualOverride) digitalWrite(PUMP_PIN, LOW);
            }
            else if (request.indexOf("GET /SET_GLOBAL") >= 0) {
              if (!isScheduleLocked()) {
                int durIdx = request.indexOf("dur=");
                int enIdx = request.indexOf("en=");
                if (durIdx > 0) sysConfig.runDuration = request.substring(durIdx + 4, request.indexOf("&", durIdx)).toInt();
                sysConfig.globalEnabled = (enIdx > 0);
                saveConfig();
                scheduleErrorMsg = "";
              }
            }
            else if (request.indexOf("GET /SET_LIGHT_CONFIG") >= 0) {
              if (!isScheduleLocked()) {
                int leadIdx = request.indexOf("lead=");
                if (leadIdx > 0) sysConfig.lightLeadMinutes = request.substring(leadIdx + 5, request.indexOf(" ", leadIdx)).toInt();
                saveConfig();
                scheduleErrorMsg = "";
              }
            }
            else if (request.indexOf("GET /SET_SLEEP_DELAY") >= 0) {
              int sldIdx = request.indexOf("sld=");
              if (sldIdx > 0) sysConfig.fallingAsleepMinutes = request.substring(sldIdx + 4, request.indexOf(" ", sldIdx)).toInt();
              saveConfig();
            }
            else if (request.indexOf("GET /CALC_BED") >= 0) {
              int hIdx = request.indexOf("wh=");
              int mIdx = request.indexOf("wm=");
              if (hIdx > 0 && mIdx > 0) {
                targetWakeH = request.substring(hIdx + 3, request.indexOf("&", hIdx)).toInt();
                targetWakeM = request.substring(mIdx + 3, request.indexOf(" ", mIdx)).toInt();
                showBedTimes = true;
              }
            }
            else if (request.indexOf("GET /SET_SCHEDULE") >= 0) {
              if (!isScheduleLocked()) {
                for (int i = 0; i < 7; i++) {
                  String hK = "h" + String(i) + "=";
                  String mK = "m" + String(i) + "=";
                  String aK = "a" + String(i) + "=";
                  if (request.indexOf(hK) > 0) sysConfig.schedule[i].hour = request.substring(request.indexOf(hK) + hK.length()).toInt();
                  if (request.indexOf(mK) > 0) sysConfig.schedule[i].minute = request.substring(request.indexOf(mK) + mK.length()).toInt();
                  sysConfig.schedule[i].active = (request.indexOf(aK) > 0);
                }
                saveConfig();
                scheduleErrorMsg = "";
              }
            }

            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();
            client.println("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
            client.println("<style>");
            client.println("body{font-family:-apple-system,system-ui,sans-serif;text-align:center;margin:0;padding:10px;background-color:#efeff4;color:#333;}.card{background:white;padding:15px;margin:15px auto;max-width:500px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.05);}h2{font-size:1.1rem;margin-top:0;color:#666;border-bottom:1px solid #eee;padding-bottom:8px;text-transform:uppercase;}h3{font-size:0.9rem;color:#888;margin:15px 0 5px 0;text-align:left;border-left:3px solid #007aff;padding-left:8px;}.row{display:flex;align-items:center;justify-content:space-between;padding:10px 0;border-bottom:1px solid #f0f0f0;}input[type=number]{padding:8px;width:50px;text-align:center;font-size:16px;border:1px solid #ccc;border-radius:6px;}.btn{width:100%;padding:15px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;margin-top:10px;}.btn-save{background-color:#007aff;color:white;}.btn-manual{background-color:#ff3b30;color:white;}.btn-stop{background-color:#8e8e93;color:white;}.cycle-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px;}.cycle-item{background:#f8f8f8;padding:10px;border-radius:8px;font-size:0.85rem;}.cycle-time{font-weight:bold;color:#007aff;display:block;font-size:1.1rem;}.calc-row{display:flex;align-items:center;justify-content:flex-start;gap:5px;padding:10px 0;}.error-msg{background:#ff3b30;color:white;padding:12px;border-radius:8px;margin:15px auto;max-width:500px;font-weight:bold;}</style>");

            bool scheduleLocked = isScheduleLocked();
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

            client.println("<div class=\"card\"><h2>Weekly Schedule</h2>");
            client.println("<form action=\"/SET_SCHEDULE\" method=\"GET\">" + hp);
            for(int i=0; i<7; i++) {
              String disabledAttr = scheduleLocked ? " disabled" : "";
              client.println("<div class=\"row\"><b>"+String(daysOfWeek[i])+"</b><div><input type=\"number\" name=\"h"+String(i)+"\" min=\"0\" max=\"23\" value=\""+String(sysConfig.schedule[i].hour)+"\""+disabledAttr+"> : <input type=\"number\" name=\"m"+String(i)+"\" min=\"0\" max=\"59\" value=\""+String(sysConfig.schedule[i].minute)+"\""+disabledAttr+"><input type=\"checkbox\" name=\"a"+String(i)+"\" "+(sysConfig.schedule[i].active?"checked":"")+" style=\"width:20px;height:20px;margin-left:8px\""+disabledAttr+"></div></div>");
            }
            if(scheduleLocked) {
              client.println("<button type=\"button\" class=\"btn\" style=\"background:#ccc;cursor:not-allowed\" disabled>SAVE SCHEDULE</button></form></div>");
            } else {
              client.println("<button type=\"submit\" class=\"btn btn-save\">SAVE SCHEDULE</button></form></div>");
            }

            client.println("<div class=\"card\"><h2>Settings</h2>");
            client.println("<form action=\"/SET_GLOBAL\" method=\"GET\">" + hp);
            String disabledAttr = scheduleLocked ? " disabled" : "";
            client.println("<div class=\"row\"><span>Pump Duration (sec)</span><input type=\"number\" name=\"dur\" value=\""+String(sysConfig.runDuration)+"\""+disabledAttr+"></div><div class=\"row\"><span>System Enabled</span><input type=\"checkbox\" name=\"en\" "+String(sysConfig.globalEnabled?"checked":"")+" style=\"width:20px;height:20px\""+disabledAttr+"></div>");
            if(scheduleLocked) {
              client.println("<button type=\"button\" class=\"btn\" style=\"background:#ccc;cursor:not-allowed\" disabled>UPDATE SETTINGS</button></form></div>");
            } else {
              client.println("<button type=\"submit\" class=\"btn btn-save\">UPDATE SETTINGS</button></form></div>");
            }

            client.println("<div class=\"card\"><h2>Light Settings</h2>");
            client.println("<form action=\"/SET_LIGHT_CONFIG\" method=\"GET\">" + hp + "<div class=\"row\"><span>Lead Time (minutes)</span><input type=\"number\" name=\"lead\" value=\""+String(sysConfig.lightLeadMinutes)+"\""+disabledAttr+"></div>");
            if(scheduleLocked) {
              client.println("<button type=\"button\" class=\"btn\" style=\"background:#ccc;cursor:not-allowed\" disabled>UPDATE LIGHT CONFIG</button></form></div>");
            } else {
              client.println("<button type=\"submit\" class=\"btn btn-save\">UPDATE LIGHT CONFIG</button></form></div>");
            }

            client.println("<div class=\"card\"><h2>Sleep Cycles</h2><form action=\"/SET_SLEEP_DELAY\" method=\"GET\">" + hp + "<div class=\"row\" style=\"border-bottom:1px solid #eee\"><span>Time to fall asleep (min)</span><div style=\"display:flex;gap:5px\"><input type=\"number\" name=\"sld\" value=\""+String(sysConfig.fallingAsleepMinutes)+"\" min=\"0\"><button type=\"submit\" style=\"padding:8px; border-radius:6px; border:none; background:#007aff; color:white\">OK</button></div></div></form><h3>If you go to sleep now</h3><div class=\"cycle-grid\">");
            float opts[] = {10.5, 9.0, 7.5, 6.0};
            for(int i=0; i<4; i++) client.println("<div class=\"cycle-item\">"+String(opts[i],1)+"h ("+String((int)(opts[i]/1.5))+" cycles)<span class=\"cycle-time\">"+getWakeTime(opts[i])+"</span></div>");
            client.println("</div><h3>If you want to wake up at...</h3><form action=\"/CALC_BED\" method=\"GET\">" + hp + "<div class=\"calc-row\"><input type=\"number\" name=\"wh\" value=\""+String(targetWakeH)+"\" min=\"0\" max=\"23\"> : <input type=\"number\" name=\"wm\" value=\""+String(targetWakeM)+"\" min=\"0\" max=\"59\"><button type=\"submit\" style=\"padding:10px 15px; border-radius:6px; border:none; background:#007aff; color:white; font-weight:bold\">CALCULATE</button></div></form>");
            if(showBedTimes){
              client.println("<div class=\"cycle-grid\">");
              for(int i=0; i<4; i++) client.println("<div class=\"cycle-item\">Sleep "+String(opts[i],1)+"h<span class=\"cycle-time\">"+getBedTime(targetWakeH, targetWakeM, opts[i])+"</span></div>");
              client.println("</div>");
            }
            client.println("</div>");

            client.print("<div class=\"card\"><h2>Status</h2><p>"+String(daysOfWeek[timeClient.getDay()])+", "+timeClient.getFormattedTime()+"</p><p style=\"font-weight:bold\">"+String(manualOverride?"<span style='color:#ff3b30'>MANUAL ON</span>":(digitalRead(PUMP_PIN)?"RUNNING":"IDLE"))+"</p>");
            client.print("<a href=\"/TOGGLE?" + pinParam + "\" "+String(manualOverride?"":"onclick=\"return confirm('Confirm manual pump start?')\"")+"><button class=\"btn "+String(manualOverride?"btn-stop":"btn-manual")+"\">"+String(manualOverride?"STOP PUMP" : "MANUAL START")+"</button></a></div></body></html>");
            break;
          } else currentLine = "";
        } else if (c != '\r') currentLine += c;
      }
    }
    client.stop();
  }

  if (manualOverride) digitalWrite(PUMP_PIN, HIGH);
  else {
    int curDay = timeClient.getDay();
    int curTotal = timeClient.getHours() * 60 + timeClient.getMinutes();
    int almTotal = sysConfig.schedule[curDay].hour * 60 + sysConfig.schedule[curDay].minute;
    int lgtTotal = almTotal - sysConfig.lightLeadMinutes;
    int lgtDay = (lgtTotal < 0) ? ((curDay == 0) ? 6 : curDay - 1) : curDay;
    if (lgtTotal < 0) lgtTotal += 1440;

    if (sysConfig.globalEnabled) {
      if (sysConfig.schedule[lgtDay].active && curTotal == lgtTotal && !lightTriggered) { triggerVoiceMonkey(); lightTriggered = true; }
      if (sysConfig.schedule[curDay].active && curTotal == almTotal) {
        if (!alarmTriggered) { digitalWrite(PUMP_PIN, HIGH); delay(sysConfig.runDuration * 1000); digitalWrite(PUMP_PIN, LOW); alarmTriggered = true; }
      } else { alarmTriggered = false; if (curTotal != lgtTotal) lightTriggered = false; }
    } else { digitalWrite(PUMP_PIN, LOW); alarmTriggered = false; lightTriggered = false; }
  }
}
