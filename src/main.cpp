#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Adafruit_NeoPixel.h>

#define DEVICE_NAME        "pulleys"
#define MANUFACTURER_ID    0xFFFF   // 0xFFFF = test/development use
#define BEACON_INTERVAL_MS 500

#define RGB_CONTROL_PIN    14
#define RGB_COUNT          64
#define MAX_BRIGHTNESS     25

Adafruit_NeoPixel pixels(RGB_COUNT, RGB_CONTROL_PIN, NEO_RGB + NEO_KHZ800);

// Advertising interval in BLE units (0.625 ms per unit)
#define BLE_INTERVAL_UNITS (BEACON_INTERVAL_MS * 1000 / 625)  // = 800

static NimBLEAdvertising* pAdv = nullptr;
static uint32_t counter = 0;

static void updatePayload() {
  NimBLEAdvertisementData data;
  data.setName(DEVICE_NAME);

  // Manufacturer data: 2-byte company ID + 4-byte counter
  uint8_t mfr[6];
  mfr[0] = MANUFACTURER_ID & 0xFF;
  mfr[1] = (MANUFACTURER_ID >> 8) & 0xFF;
  mfr[2] = (counter >>  0) & 0xFF;
  mfr[3] = (counter >>  8) & 0xFF;
  mfr[4] = (counter >> 16) & 0xFF;
  mfr[5] = (counter >> 24) & 0xFF;
  data.setManufacturerData(std::string((char*)mfr, sizeof(mfr)));

  pAdv->setAdvertisementData(data);
}

void setup() {
  Serial.begin(115200);
  Serial.println("BOOT OK");

  NimBLEDevice::init(DEVICE_NAME);
  pAdv = NimBLEDevice::getAdvertising();
  pAdv->setMinInterval(BLE_INTERVAL_UNITS);
  pAdv->setMaxInterval(BLE_INTERVAL_UNITS);

  pixels.begin();
  pixels.setBrightness(MAX_BRIGHTNESS);
  pixels.clear();
  pixels.setPixelColor(0, pixels.Color(0, 50, 100));
  pixels.show();

  updatePayload();
  pAdv->start();
  Serial.println("BLE beacon started");
}

void loop() {
  static uint32_t lastTx = 0;
  uint32_t now = millis();

  if (now - lastTx >= BEACON_INTERVAL_MS) {
    lastTx = now;
    counter++;

    pAdv->stop();
    updatePayload();
    pAdv->start();

    // Advance lit LED on each beacon
    uint8_t ledPos = counter % RGB_COUNT;
    pixels.clear();
    pixels.setPixelColor(ledPos, pixels.Color(0, 50, 100));
    pixels.show();

    Serial.printf("Beacon #%lu  LED %d\n", counter, ledPos);
  }
}
