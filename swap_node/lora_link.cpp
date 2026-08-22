#include "lora_link.h"

// Instantiate the SX1262 module using pins mapped in config.h
SX1262 lora = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

static LoraMetrics metrics;

// ---- IN865 duty-cycle guard (rolling 1-hour TX-airtime budget) -----------
// Tracked here rather than in a header-exposed class since nothing outside
// this file needs it; sendLoRaTelemetry() is the only caller.
namespace {
constexpr uint32_t DUTY_WINDOW_MS = 3600000UL;  // 1 hour
constexpr size_t DUTY_MAX_RECORDS = 256;
struct DutyRecord { uint32_t timestamp_ms; uint32_t air_time_ms; };
DutyRecord dutyRecords[DUTY_MAX_RECORDS];
size_t dutyCount = 0;

void purgeOldDutyRecords() {
    const uint32_t now = millis();
    size_t write = 0;
    for (size_t i = 0; i < dutyCount; i++) {
        if (now - dutyRecords[i].timestamp_ms < DUTY_WINDOW_MS) {
            dutyRecords[write++] = dutyRecords[i];
        }
    }
    dutyCount = write;
}

uint32_t dutyAirtimeUsedMs() {
    uint32_t total = 0;
    for (size_t i = 0; i < dutyCount; i++) total += dutyRecords[i].air_time_ms;
    return total;
}

bool dutyCanTransmit(uint32_t nextAirTimeMs) {
    purgeOldDutyRecords();
    const uint32_t budget = static_cast<uint32_t>(DUTY_WINDOW_MS * MAX_LORA_DUTY_CYCLE);
    return (dutyAirtimeUsedMs() + nextAirTimeMs) <= budget;
}

void dutyRecordTransmission(uint32_t airTimeMs) {
    if (dutyCount < DUTY_MAX_RECORDS) {
        dutyRecords[dutyCount].timestamp_ms = millis();
        dutyRecords[dutyCount].air_time_ms = airTimeMs;
        dutyCount++;
    }
}

// Rough SF/BW engineering estimate for duty-cycle budgeting only — not a
// certified regulatory air-time calculator.
uint32_t estimateAirTimeMs(size_t payloadLen) {
    // Semtech's airtime formula's "CR" term is the coding-rate numerator
    // offset (1-4, where actual rate = 4/(4+CR)). LORA_CR is in RadioLib's
    // convention instead (5-8, the denominator of 4/5..4/8, matching what
    // lora.begin() expects) — convert before using it here, or this
    // overestimates airtime and over-throttles duty-cycle budgeting.
    constexpr float codingRateSemtech = static_cast<float>(LORA_CR - 4);

    const float symbolTimeMs = static_cast<float>(1UL << LORA_SF) / LORA_BW;
    const float nSymbols = 8.0f + max(
        0.0f,
        ceilf((8.0f * payloadLen - 4.0f * LORA_SF + 28.0f + 16.0f) / (4.0f * LORA_SF)) * codingRateSemtech);
    constexpr float preambleSymbols = 8.0f;
    return static_cast<uint32_t>((preambleSymbols + 4.25f + nSymbols) * symbolTimeMs);
}
}  // namespace

// ---- Interrupt-driven receive ---------------------------------------------
static volatile bool loraPacketReceived = false;

static void IRAM_ATTR onLoraDio1() {
    // Never call Serial/String/SPI/RadioLib APIs from inside an ISR.
    loraPacketReceived = true;
}

void setupLoRa() {
    Serial.print("[LoRa] Initializing SX1262... ");

    // CRITICAL FIX: Explicitly assign RXEN and TXEN to control the RF switch.
    // Without this, the SX1262 will report success but transmit into a dead end.
    lora.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

    // Initialize radio with IN865 compliant settings
    int state = lora.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_PWR);

    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("Success!");
    } else {
        Serial.print("Failed, code ");
        Serial.println(state);
    }

    lora.setDio1Action(onLoraDio1);
}

bool sendLoRaTelemetry(const char* payload) {
    const size_t len = strlen(payload);
    const uint32_t airTime = estimateAirTimeMs(len);
    if (!dutyCanTransmit(airTime)) {
        Serial.println("[LoRa] duty-cycle budget exceeded, TX suppressed");
        metrics.packets_lost++;
        return false;
    }

    const uint32_t start = millis();
    const int state = lora.transmit(payload);
    metrics.last_rtt_ms = millis() - start;

    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("[LoRa] Transmit successful.");
        metrics.packets_sent++;
        dutyRecordTransmission(airTime);
        return true;
    }

    Serial.print("[LoRa] Transmit failed, code ");
    Serial.println(state);
    metrics.packets_lost++;
    return false;
}

bool receiveLoRaTelemetry(uint8_t* buf, size_t bufLen, size_t& receivedLen, uint32_t timeoutMs) {
    receivedLen = 0;
    loraPacketReceived = false;

    const int startState = lora.startReceive();
    if (startState != RADIOLIB_ERR_NONE) {
        Serial.print("[LoRa] startReceive failed, code ");
        Serial.println(startState);
        return false;
    }

    const uint32_t waitStart = millis();
    while (!loraPacketReceived && (millis() - waitStart) < timeoutMs) {
        delay(2);
    }
    if (!loraPacketReceived) {
        return false;  // timeout — expected whenever the peer isn't transmitting
    }
    loraPacketReceived = false;

    const size_t length = lora.getPacketLength();
    if (length == 0 || length > bufLen) {
        Serial.println("[LoRa] Invalid/oversized packet length.");
        return false;
    }

    const int readState = lora.readData(buf, length);
    if (readState != RADIOLIB_ERR_NONE) {
        Serial.print("[LoRa] readData failed, code ");
        Serial.println(readState);
        return false;
    }

    receivedLen = length;
    metrics.rssi_dbm = lora.getRSSI();
    metrics.snr_db = lora.getSNR();
    return true;
}

const LoraMetrics& getLoraMetrics() {
    return metrics;
}
