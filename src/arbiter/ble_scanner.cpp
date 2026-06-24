#include "ble_scanner.h"
#include <NimBLEDevice.h>
#include <Arduino.h>
#include <string.h>

static ArbiterDevice s_devices[ARBITER_MAX_DEVICES];

// Find existing slot by MAC, or claim the oldest/free one
static ArbiterDevice* find_or_alloc(const uint8_t* mac) {
    ArbiterDevice* victim = nullptr;
    uint32_t oldest_ms = UINT32_MAX;

    for (int i = 0; i < ARBITER_MAX_DEVICES; i++) {
        if (s_devices[i].active && memcmp(s_devices[i].mac, mac, 6) == 0)
            return &s_devices[i];
        if (!s_devices[i].active) {
            victim = &s_devices[i];
            oldest_ms = 0;
        } else if (s_devices[i].last_seen_ms < oldest_ms) {
            oldest_ms = s_devices[i].last_seen_ms;
            victim = &s_devices[i];
        }
    }
    if (victim) {
        memset(victim, 0, sizeof(ArbiterDevice));
        memcpy(victim->mac, mac, 6);
        victim->active = true;
        victim->first_seen_ms = millis();
    }
    return victim;
}

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveManufacturerData()) return;
        std::string raw = dev->getManufacturerData();
        PulleysPacket pkt;
        if (!pulleys_parse((const uint8_t*)raw.data(), raw.size(), &pkt)) return;
        if (pkt.deviceType != PULLEYS_TYPE_TRAVELER &&
            pkt.deviceType != PULLEYS_TYPE_STATION) return;

        uint8_t mac[6];
        memcpy(mac, dev->getAddress().getBase()->val, 6);

        ArbiterDevice* d = find_or_alloc(mac);
        if (!d) return;

        d->type         = pkt.deviceType;
        d->id           = pkt.deviceId;
        d->culture      = pkt.culture;
        d->current_rssi = (int8_t)dev->getRSSI();
        d->last_seen_ms = millis();
    }
};

void scanner_init() {
    memset(s_devices, 0, sizeof(s_devices));
    NimBLEDevice::init("");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(new ScanCallbacks(), true);
    scan->setActiveScan(false);
    scan->setInterval(160);   // 100ms window
    scan->setWindow(80);      // 50ms active
    scan->start(0, false, false);
    Serial.println("  [BLE] Scanner started");
}

void scanner_tick() {
    uint32_t now = millis();
    for (int i = 0; i < ARBITER_MAX_DEVICES; i++) {
        ArbiterDevice& d = s_devices[i];
        if (!d.active) continue;

        if ((now - d.last_seen_ms) > DEVICE_TIMEOUT_MS) {
            d.active = false;
            continue;
        }

        // Push a 1-second RSSI sample into the ring buffer.
        // If the device wasn't seen recently, push -127 as a gap marker.
        int8_t sample = ((now - d.last_seen_ms) < 2000) ? d.current_rssi : -127;
        d.rssi_buf[d.rssi_head] = sample;
        d.rssi_head = (d.rssi_head + 1) % RSSI_HISTORY_LEN;
        if (d.rssi_count < RSSI_HISTORY_LEN) d.rssi_count++;
    }
}

const ArbiterDevice* scanner_devices() { return s_devices; }

uint8_t scanner_count() {
    uint8_t n = 0;
    for (int i = 0; i < ARBITER_MAX_DEVICES; i++)
        if (s_devices[i].active) n++;
    return n;
}
