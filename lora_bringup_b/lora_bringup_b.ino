// Minimal LoRa bring-up: Node B (receiver).
//
// Listens for Node A's counter payload over LoRa and logs what it received
// (sequence number, RSSI, SNR) over Serial. No WiFi, no BLE, no adaptive
// switching, no OLED -- pairs with lora_bringup_a/lora_bringup_a.ino to
// prove the SX1262 point-to-point link works before layering swap_node's
// full adaptive-switching logic back on top.
//
// Two-way ack: after each successful receive, Node B echoes a "PONG <seq>"
// reply back to Node A so A can time the round trip. Node B itself can't
// measure a true cross-node round-trip (it never sees its own PONG's
// flight time back to A) -- what it logs is local reply latency (PING
// parsed -> PONG handed to the radio), the closest per-exchange timing
// signal available on this side.
#include <stdio.h>
#include "config.h"
#include "lora_link.h"

static uint32_t receivedCount = 0;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("=== LoRa bring-up: Node B (RX) ===");

    setupLoRa();
}

void loop() {
    uint8_t buf[64];
    size_t len = 0;
    // Blocks up to 1s waiting for a packet; a timeout here is the expected
    // steady state whenever Node A isn't transmitting mid-cycle, not an error.
    const bool ok = receiveLoRaTelemetry(buf, sizeof(buf) - 1, len, 1000);

    if (!ok) {
        return;
    }

    buf[len] = '\0';
    receivedCount++;
    Serial.print("[B] rx #");
    Serial.print(receivedCount);
    Serial.print(": ");
    Serial.println((char*)buf);

    const LoraMetrics& m = getLoraMetrics();
    Serial.print("[B] RSSI: ");
    Serial.print(m.rssi_dbm);
    Serial.print(" dBm  SNR: ");
    Serial.print(m.snr_db);
    Serial.println(" dB");

    // Pull the seq back out of "PING <seq>" and echo it straight back as
    // "PONG <seq>" so Node A can match the reply to the send it's timing.
    unsigned long pingSeq = 0;
    if (sscanf((char*)buf, "PING %lu", &pingSeq) == 1) {
        char reply[32];
        snprintf(reply, sizeof(reply), "PONG %lu", pingSeq);

        const uint32_t replyStartMs = millis();
        const bool ackSent = sendLoRaTelemetry(reply);
        const uint32_t replyLatencyMs = millis() - replyStartMs;

        Serial.print("[B] ack seq=");
        Serial.print(pingSeq);
        Serial.print(" sent=");
        Serial.print(ackSent ? "ok" : "FAILED");
        Serial.print(" latency=");
        Serial.print(replyLatencyMs);
        Serial.println("ms");
    } else {
        Serial.println("[B] payload not a recognized PING, no ack sent");
    }
}
