#include <Arduino.h>
#include <NimBLEDevice.h>
#include <pulleys_protocol.h>

class TravelerCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    if (!dev->haveName() ||
        String(dev->getName().c_str()) != PULLEYS_DEVICE_NAME) return;
    if (!dev->haveManufacturerData()) return;

    std::string raw = dev->getManufacturerData();
    PulleysPacket pkt;
    if (pulleys_parse((const uint8_t*)raw.data(), raw.size(), &pkt)) {
      Serial.printf("Traveler  counter=%-6lu  RSSI=%d dBm\n",
                    pkt.counter, dev->getRSSI());
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("Station BOOT OK");

  NimBLEDevice::init("");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new TravelerCallbacks(), true);
  pScan->setActiveScan(false);   // passive — no need to request scan response
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(0, nullptr, false);  // scan forever

  Serial.println("Scanning for Travelers...");
}

void loop() {
  delay(1000);
}
