#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#define RGB_Control_PIN   14
#define Matrix_Row        8
#define Matrix_Col        8
#define RGB_COUNT         64

#define MAX_BRIGHTNESS    25  // 10% of 255

Adafruit_NeoPixel pixels(RGB_COUNT, RGB_Control_PIN, NEO_RGB + NEO_KHZ800);

void setup() {
  Serial.begin(115200);
  Serial.println("BOOT OK");

  pixels.begin();
  pixels.setBrightness(MAX_BRIGHTNESS);
  pixels.clear();
  pixels.show();
}

void loop() {
  for (int i = 0; i < RGB_COUNT; i++) {
    pixels.clear();
    pixels.setPixelColor(i, pixels.Color(255, 0, 0));
    pixels.show();
    Serial.printf("LED %d\n", i);
    delay(200);
  }
}
