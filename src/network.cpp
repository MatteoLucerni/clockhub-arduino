#include "network.h"
#include "globals.h"
#include "config.h"
#include <WiFiS3.h>

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

void setupWiFi() {
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
}

void updateDuckDNSIfNeeded() {
  if (millis() - lastDuckDNSUpdate >= DUCKDNS_INTERVAL) {
    updateDuckDNS();
    lastDuckDNSUpdate = millis();
  }
}
