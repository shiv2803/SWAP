#ifndef SWAP_LINK_MANAGER_H
#define SWAP_LINK_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include "config.h"
#include "lora_link.h"

enum class ActiveProtocol : uint8_t { WIFI = 0, BLE = 1, LORA = 2 };

struct LinkQualitySample {
    float rssi_dbm = -999.0f;
    float snr_db = 0.0f;   // LoRa only, else 0
    uint32_t rtt_ms = 0;
    bool packet_ok = false;
};

// Fixed circular buffer of the last METRIC_WINDOW_SAMPLES exchanges for one
// protocol. Used both to drive evaluateAndSwitch() and to report
// wifi_rssi/wifi_loss/ble_rssi/lora_rssi/lora_loss telemetry.
class RollingMetrics {
public:
    void push(const LinkQualitySample& s) {
        buf_[head_] = s;
        head_ = (head_ + 1) % METRIC_WINDOW_SAMPLES;
        if (count_ < METRIC_WINDOW_SAMPLES) count_++;
    }

    float avgRssi() const {
        if (count_ == 0) return -999.0f;
        float sum = 0;
        for (uint8_t i = 0; i < count_; i++) sum += buf_[i].rssi_dbm;
        return sum / count_;
    }

    float avgRtt() const {
        if (count_ == 0) return 0.0f;
        float sum = 0;
        for (uint8_t i = 0; i < count_; i++) sum += buf_[i].rtt_ms;
        return sum / count_;
    }

    float packetLossRate() const {
        if (count_ == 0) return 1.0f;
        uint8_t lost = 0;
        for (uint8_t i = 0; i < count_; i++) if (!buf_[i].packet_ok) lost++;
        return static_cast<float>(lost) / count_;
    }

    uint8_t sampleCount() const { return count_; }

private:
    LinkQualitySample buf_[METRIC_WINDOW_SAMPLES];
    uint8_t head_ = 0;
    uint8_t count_ = 0;
};

class LinkManager {
public:
    void begin();
    void loop();
    ActiveProtocol currentProtocol() const { return active_; }
    void emitTelemetryFrame();  // sends one JSON line over TELEMETRY_SERIAL

private:
    ActiveProtocol active_ = ActiveProtocol::WIFI;
    RollingMetrics wifiMetrics_, bleMetrics_, loraMetrics_;

    // Command channel from UNO Q ({"cmd":"force_protocol","protocol":N}\n).
    // -1 = no override, follow evaluateAndSwitch() normally.
    int forcedProtocol_ = -1;
    String commandLineBuf_;

    WiFiServer wifiTcpServer_{WIFI_PORT};
    WiFiClient wifiClient_;

    NimBLEServer* bleServer_ = nullptr;
    NimBLECharacteristic* bleChar_ = nullptr;
    NimBLEClient* bleClient_ = nullptr;
    NimBLERemoteCharacteristic* bleRemoteChar_ = nullptr;

    void beginWifi();
    void beginBle();
    bool tryWifiExchange();
    bool tryBleExchange();
    bool tryLoraExchange();
    void evaluateAndSwitch();
    void setActiveProtocolLed(ActiveProtocol p);
    void pollIncomingCommands();
};

#endif  // SWAP_LINK_MANAGER_H
