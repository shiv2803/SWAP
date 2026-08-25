#ifndef SWAP_CONFIG_H
#define SWAP_CONFIG_H

#include <Arduino.h>

// --- NODE IDENTITY ---
// Exactly one board is Node A, the other Node B. link_manager.cpp and
// swap_node.ino branch on #ifdef NODE_ROLE_A (presence, not value) — so to
// build Node B's image, comment out the #define below entirely. Leaving it
// defined (even as 0) still makes every #ifdef NODE_ROLE_A branch true,
// which is what silently broke role selection before this fix.
// #define NODE_ROLE_A

// --- WIFI LINK (Node A hosts the SoftAP, Node B connects as a station) ---
#define WIFI_SSID "SWAP_LINK"
#define WIFI_PASS "swap-link-2026"  // WPA2 requires >= 8 chars
#define WIFI_PORT 8080
#define WIFI_CONNECT_TIMEOUT_MS 8000

// --- BLE CONFIGURATION ---
#define BLE_DEVICE_NAME "SWAP_NODE"
#define BLE_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
// Single characteristic, NOTIFY (Node A -> Node B) + WRITE (Node B -> Node A)
// — link_manager.cpp only ever creates/looks up one characteristic.
#define BLE_CHAR_UUID "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

// --- Connection Parameters ---
#define BLE_MIN_CONN_INTERVAL 12  // 15ms
#define BLE_MAX_CONN_INTERVAL 12
#define BLE_SCAN_TIME_S 3         // active scan duration
#define BLE_RECONNECT_INTERVAL_MS 3000
#define BLE_EXCHANGE_TIMEOUT_MS 300

// --- LoRa (SX1262) pin map ---
// Taken from the real, physical pin diagram (SWAP Project Node A/B ESP32
// DevKit V1 38-pin diagrams, silkscreen-matched) -- these were previously
// unverified placeholders and were WRONG; corrected against that diagram.
// SPI bus itself (MOSI=23, MISO=19, SCK=18) matches RadioLib's default
// VSPI pins, so those are left implicit rather than redefined here.
#define LORA_NSS 21    // SPI_NSS / LoRa CS
#define LORA_RST 33    // LORA_RESET
#define LORA_BUSY 27   // LORA_BUSY
#define LORA_DIO1 26   // LORA_DIO1 (IRQ)
#define LORA_RXEN 25   // LORA_RXEN (must be driven in firmware)
#define LORA_TXEN 32   // LORA_TXEN (must be driven in firmware)

// --- LoRa radio parameters (IN865 region defaults) ---
// PWR is a conservative default, not a regulatory certification — check your
// local IN865 EIRP limit before increasing it.
#define LORA_FREQ 865.0     // MHz
#define LORA_BW 125.0       // kHz
#define LORA_SF 9
#define LORA_CR 7           // 4/7
#define LORA_SYNC 0x12      // RadioLib private-network sync word
#define LORA_PWR 14         // dBm
// IN865 primary sub-band duty cycle allowance (verify against the exact
// sub-band you transmit on — some IN865 sub-bands permit more).
#define MAX_LORA_DUTY_CYCLE 0.01f

// --- Telemetry UART (Serial2 -> UNO Q D14/D15, see uno_q_mcu_sketch) ---
// 115200, not 19200 -- UNO Q's D0/D1 (usart1) doubles as the Zephyr
// console/boot-log UART on that side and holds 115200 regardless of what's
// requested there, per uno_q_mcu_sketch/sketch/sketch.ino's own comment.
// Matching it here is the only way both ends agree; the old 19200 meant
// zero valid frames were ever received.
#define TELEMETRY_SERIAL Serial2
#define TELEMETRY_BAUD 115200
#define TELEMETRY_RX_PIN 16
#define TELEMETRY_TX_PIN 17

// --- Status LEDs: REMOVED (2026-08-23) ---
// The real pin diagram confirms this board's design dropped discrete status
// LEDs entirely -- GPIO4/13/27 (previously LED_WIFI/LED_BLE/LED_LORA here)
// are the I2C bus and LORA_BUSY. Status is read via Serial / telemetry only
// now -- no on-board indicator.

// --- Link exchange / switching tuning ---
// Per-exchange timeout, shared by the Wi-Fi/BLE/LoRa PING-PONG round trips.
#define LINK_TIMEOUT_MS 300
// How often Node A emits its own telemetry AND (if it has heard from Node B
// recently) Node B's relayed telemetry, over TELEMETRY_SERIAL to the UNO Q.
#define TELEMETRY_INTERVAL_MS 4000
// Rolling metrics window size — matches swap_backend's LinkQualityModel
// window_size=10 so both sides reason over comparably-fresh data.
#define METRIC_WINDOW_SAMPLES 10
// Mirrors swap_backend/link_quality_model.py's RuleBasedFallback thresholds
// (WIFI_RSSI_MIN/WIFI_LOSS_MAX/BLE_RSSI_MIN) so the firmware's own
// evaluateAndSwitch() agrees with the backend's fallback rule.
#define WIFI_RSSI_MIN_DBM -75.0f
#define WIFI_PACKET_LOSS_MAX 0.10f
#define BLE_RSSI_MIN_DBM -85.0f

#endif
