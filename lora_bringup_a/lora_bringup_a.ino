// Minimal LoRa bring-up: Node A (transmitter).
//
// Sends a counter payload over LoRa once a second and logs send/lost
// counts and round-trip time over Serial. No WiFi, no BLE, no adaptive
// switching, no OLED -- this exists purely to prove the SX1262
// point-to-point link between two physical boards actually works before
// layering swap_node's full adaptive-switching logic back on top.
//
// Two-way ack: after each PING send, Node A waits (with a short timeout)
// for Node B's "PONG <seq>" reply and times the full round trip. A missed
// ack is expected occasionally (radio contention, B mid-boot) and is not
// itself a link failure -- only a run of misses is worth investigating.
#include <string.h>
#include "config.h"
#include "lora_link.h"

static uint32_t seq = 0;
static const uint32_t SEND_INTERVAL_MS = 1000;
static const uint32_t ACK_TIMEOUT_MS = 1500;
static uint32_t lastSendMs = 0;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("=== LoRa bring-up: Node A (TX) ===");

    setupLoRa();
}

void loop() {
    const uint32_t now = millis();
    if (now - lastSendMs < SEND_INTERVAL_MS) {
        return;
    }
    lastSendMs = now;

    seq++;
    char payload[32];
    snprintf(payload, sizeof(payload), "PING %lu", (unsigned long)seq);

    const uint32_t sendStartMs = millis();
    const bool sent = sendLoRaTelemetry(payload);
    Serial.print("[A] seq=");
    Serial.print(seq);
    Serial.print(" sent=");
    Serial.println(sent ? "ok" : "FAILED");

    if (sent) {
        // Wait for Node B's PONG reply to this seq. A timeout here is a
        // lost ack, not a link failure -- expected occasionally.
        uint8_t buf[64];
        size_t len = 0;
        if (receiveLoRaTelemetry(buf, sizeof(buf) - 1, len, ACK_TIMEOUT_MS)) {
            buf[len] = '\0';
            char expected[32];
            snprintf(expected, sizeof(expected), "PONG %lu", (unsigned long)seq);
            if (strcmp((char*)buf, expected) == 0) {
                const uint32_t rttMs = millis() - sendStartMs;
                Serial.print("[A] ack seq=");
                Serial.print(seq);
                Serial.print(" rtt=");
                Serial.print(rttMs);
                Serial.println("ms");
            } else {
                Serial.print("[A] unexpected reply: ");
                Serial.println((char*)buf);
            }
        } else {
            Serial.println("[A] no ack (timeout)");
        }
    }

    const LoraMetrics& m = getLoraMetrics();
    Serial.print("[A] stats sent=");
    Serial.print(m.packets_sent);
    Serial.print(" lost=");
    Serial.println(m.packets_lost);
}
