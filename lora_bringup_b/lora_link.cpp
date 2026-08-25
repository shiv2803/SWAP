#include "lora_link.h"

// Instantiate the SX1262 module using pins mapped in config.h
SX1262 lora = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

static LoraMetrics metrics;

// ---- IN865 duty-cycle guard (rolling 1-hour TX-airtime budget) -----------
// Tracked here rather than in a header-exposed class since nothing outside
// this file needs it; sendLoRaTelemetry() is the only caller.
namespace {
constexpr uint32_t DUTY_WINDOW_MS = 3600000UL;  // 1 hour
// Sized for the two-way ack test's ~1 exchange/second cadence (each exchange
// records up to 2 transmissions -- Node A's PING and Node B's PONG). At 256
// this filled in well under 5 minutes; once full, dutyRecordTransmission()
// silently stopped recording new sends (see its "if (dutyCount <
// DUTY_MAX_RECORDS)" guard below), which let dutyCanTransmit() keep
// approving transmissions without them ever counting against the budget --
// a silent IN865 1% duty-cycle violation, not a crash, so it wouldn't have
// shown up as an error message. 4096 covers well over an hour at this
// cadence before the same ceiling could recur.
constexpr size_t DUTY_MAX_RECORDS = 4096;
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

// ---- Self-healing recovery -------------------------------------------------
// standby() (added at each receive failure exit below) recovers the common
// wedge case where the chip is fine but left mid-RX. It can't fix a radio
// that's actually stuck (e.g. BUSY line mishandling) -- for that, only a
// full reset via begin() (which re-toggles LORA_RST) helps. Track only
// *real* radio-level faults here, never an ack/RX timeout, since misses are
// an expected, normal outcome of the two-way exchange and would otherwise
// trip an unnecessary reinit under ordinary operation.
namespace {
uint8_t consecutiveRadioFailures = 0;
constexpr uint8_t MAX_CONSECUTIVE_RADIO_FAILURES = 5;

void noteRadioSuccess() {
    consecutiveRadioFailures = 0;
}
}  // namespace

void setupLoRa() {
    Serial.print("[LoRa] Initializing SX1262... ");

    // CRITICAL FIX: Explicitly assign RXEN and TXEN to control the RF switch.
    // Without this, the SX1262 will report success but transmit into a dead end.
    lora.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

    // Initialize radio with IN865 compliant settings. Preamble length and
    // TCXO voltage are passed explicitly (see config.h) rather than left to
    // RadioLib's defaults.
    int state = lora.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_PWR,
                            LORA_PREAMBLE_LEN, LORA_TCXO_VOLTAGE);

    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("Success!");
    } else {
        Serial.print("Failed, code ");
        Serial.println(state);
        // -2 (CHIP_NOT_FOUND) from begin() itself -- not just a receive
        // path -- means it's failing right after a fresh RST pulse, before
        // any of our own logic runs. That points at hardware (wiring,
        // power sag, a stuck BUSY line), not firmware state. BUSY should
        // read LOW once the chip is ready to accept commands; if it reads
        // HIGH here, the chip is stuck asserting busy and begin()'s
        // internal wait-for-BUSY-low will time out on every call,
        // regardless of how many times we reinit.
        Serial.print("[LoRa] BUSY pin (GPIO");
        Serial.print(LORA_BUSY);
        Serial.print(") reads: ");
        Serial.println(digitalRead(LORA_BUSY) ? "HIGH (stuck busy?)" : "LOW");
    }

    lora.setDio1Action(onLoraDio1);
    consecutiveRadioFailures = 0;
}

namespace {
// Called after standby() alone has already been tried and a real radio
// fault still happened. Escalates to a full reinit (re-toggles LORA_RST via
// begin()) after enough consecutive faults, since a chip that's truly stuck
// won't necessarily respond to standby() over SPI either.
void noteRadioFailureAndMaybeReinit() {
    consecutiveRadioFailures++;
    if (consecutiveRadioFailures >= MAX_CONSECUTIVE_RADIO_FAILURES) {
        Serial.println("[LoRa] too many consecutive radio faults, reinitializing...");
        setupLoRa();  // resets consecutiveRadioFailures back to 0
    }
}
}  // namespace

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
        noteRadioSuccess();
        return true;
    }

    Serial.print("[LoRa] Transmit failed, code ");
    Serial.println(state);
    metrics.packets_lost++;
    noteRadioFailureAndMaybeReinit();
    return false;
}

bool receiveLoRaTelemetry(uint8_t* buf, size_t bufLen, size_t& receivedLen, uint32_t timeoutMs) {
    receivedLen = 0;
    loraPacketReceived = false;

    const int startState = lora.startReceive();
    if (startState != RADIOLIB_ERR_NONE) {
        Serial.print("[LoRa] startReceive failed, code ");
        Serial.println(startState);
        lora.standby();
        noteRadioFailureAndMaybeReinit();
        return false;
    }

    const uint32_t waitStart = millis();
    while (!loraPacketReceived && (millis() - waitStart) < timeoutMs) {
        delay(2);
    }
    if (!loraPacketReceived) {
        // Timeout — expected whenever the peer isn't transmitting, so this
        // does NOT count as a radio fault. Still force the radio back to
        // standby: leaving it parked mid-RX after a timeout is what leads
        // to it going unresponsive (subsequent calls, including
        // transmit(), start failing with -2/CHIP_NOT_FOUND) after enough
        // cycles. See RadioLib #575 for the same symptom.
        lora.standby();
        return false;
    }
    loraPacketReceived = false;

    const size_t length = lora.getPacketLength();
    if (length == 0 || length > bufLen) {
        Serial.println("[LoRa] Invalid/oversized packet length.");
        lora.standby();
        noteRadioFailureAndMaybeReinit();
        return false;
    }

    const int readState = lora.readData(buf, length);
    if (readState != RADIOLIB_ERR_NONE) {
        Serial.print("[LoRa] readData failed, code ");
        Serial.println(readState);
        lora.standby();
        noteRadioFailureAndMaybeReinit();
        return false;
    }

    receivedLen = length;
    metrics.rssi_dbm = lora.getRSSI();
    metrics.snr_db = lora.getSNR();
    noteRadioSuccess();
    return true;
}

const LoraMetrics& getLoraMetrics() {
    return metrics;
}
