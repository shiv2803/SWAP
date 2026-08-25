#ifndef SWAP_LORA_LINK_H
#define SWAP_LORA_LINK_H

#include <Arduino.h>
#include <RadioLib.h>
#include "config.h"

struct LoraMetrics {
    float rssi_dbm = -999.0f;
    float snr_db = 0.0f;
    uint32_t packets_sent = 0;
    uint32_t packets_lost = 0;
    uint32_t last_rtt_ms = 0;
};

// Initialize the LoRa module and configure the RF switch pins
void setupLoRa();

// Transmit a string payload over LoRa. Returns false (without transmitting)
// if the IN865 duty-cycle budget (MAX_LORA_DUTY_CYCLE, rolling 1-hour window)
// would be exceeded.
bool sendLoRaTelemetry(const char* payload);

// Blocking receive with a timeout. Returns true and fills buf/receivedLen on
// success, and updates getLoraMetrics().rssi_dbm/snr_db from the received
// packet. Returns false on timeout or radio error (not itself an error the
// caller needs to log — LINK_TIMEOUT_MS misses are expected when the peer
// isn't transmitting).
bool receiveLoRaTelemetry(uint8_t* buf, size_t bufLen, size_t& receivedLen, uint32_t timeoutMs);

const LoraMetrics& getLoraMetrics();

#endif // SWAP_LORA_LINK_H
