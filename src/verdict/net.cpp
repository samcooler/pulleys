#include "net.h"
#include <ArduinoOTA.h>
#include <WiFi.h>
#include "ui.h"

#if __has_include("secrets.h")
#include "secrets.h"  // gitignored: WIFI_SSID, WIFI_PASS, OTA_PASS, OTA_HOST
#else
#error "copy src/verdict/secrets.h.example to src/verdict/secrets.h and fill it in"
#endif

static const char *HOSTNAME = "verdict-machine";
static bool otaStarted = false;
static bool wasConnected = false;

void netBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void netTick() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("wifi connected: %s (%s)\n", WiFi.localIP().toString().c_str(), HOSTNAME);
  } else if (!connected && wasConnected) {
    Serial.println("wifi lost, reconnecting");
  }
  wasConnected = connected;

  if (connected && !otaStarted) {
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([]() { Serial.println("OTA start"); uiOtaProgress(0); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
      uiOtaProgress(total ? done * 100 / total : 0);
    });
    ArduinoOTA.onEnd([]() { Serial.println("OTA done, rebooting"); uiOtaProgress(100); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); uiOtaError(); delay(2000); ESP.restart(); });
    ArduinoOTA.begin();
    otaStarted = true;
  }
  if (otaStarted) ArduinoOTA.handle();
}
