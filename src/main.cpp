#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

#define DEVICE_NAME        "pulleys"
#define MANUFACTURER_ID    0xFFFF   // 0xFFFF = test/development use
#define BEACON_INTERVAL_MS 500

#define RGB_CONTROL_PIN    14
#define RGB_COUNT          64
#define MAX_BRIGHTNESS     50

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
  pixels.clear();
  pixels.show();

  updatePayload();
  pAdv->start();
  Serial.println("BLE beacon started");
}

void loop() {
  static uint32_t lastTx  = 0;
  static uint32_t lastLed = 0;
  static uint16_t hueOffset = 0;
  static float    phase     = 0.0f;
  uint32_t now = millis();

  // Animate rainbow at ~20 fps
  if (now - lastLed >= 50) {
    lastLed = now;
    hueOffset += 300;       // rolls hue across full wheel slowly
    phase     += 0.18f;     // advances sine wave

    for (int i = 0; i < RGB_COUNT; i++) {
      uint16_t hue = hueOffset + (uint16_t)(i * 65536L / RGB_COUNT);
      // Sine wave modulates brightness between 40% and 100% of MAX_BRIGHTNESS
      float bri = MAX_BRIGHTNESS * (0.7f + 0.3f * sinf(i * 2.0f * (float)M_PI / 10.0f + phase));
      pixels.setPixelColor(i, pixels.gamma32(pixels.ColorHSV(hue, 255, (uint8_t)bri)));
    }
    pixels.show();
  }

  // BLE beacon every 500 ms
  if (now - lastTx >= BEACON_INTERVAL_MS) {
    lastTx = now;
    counter++;

    pAdv->stop();
    updatePayload();
    pAdv->start();

    Serial.printf("Beacon #%lu\n", counter);
  }
}
