#ifndef SWAP_CONFIG_H
#define SWAP_CONFIG_H

#include <Arduino.h>

// --- NODE IDENTITY ---
// Exactly one board is Node A, the other Node B. link_manager.cpp and
// swap_node.ino branch on #ifdef NODE_ROLE_A (presence, not value) — so to
// build Node B's image, comment out the #define below entirely. Leaving it
// defined (even as 0) still makes every #ifdef NODE_ROLE_A branch true,
// which is what silently broke role selection before this fix.
#define NODE_ROLE_A

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
// DEFAULTS FOR A GENERIC ESP32 DEVKIT — NOT VERIFIED AGAINST PHYSICAL WIRING.
// This environment has no hardware/toolchain to build against (see README
// "What I could not verify"), so these are placeholder GPIOs chosen only to
// avoid ESP32 strapping pins (0/2/12/15) and the default VSPI bus pins
// (18=SCK, 19=MISO, 23=MOSI, which RadioLib's default SPI instance already
// claims). Confirm/adjust against your actual module wiring before flashing.
#define LORA_NSS 5
#define LORA_RST 14
#define LORA_BUSY 34   // input-only pin; BUSY is MCU-side input, so this is fine
#define LORA_DIO1 35   // input-only pin; DIO1 is MCU-side input (IRQ), so this is fine
#define LORA_RXEN 25
#define LORA_TXEN 33

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

// --- Telemetry UART (Serial2 -> CP2102 -> UNO Q, see swap_backend wiring) ---
#define TELEMETRY_SERIAL Serial2
#define TELEMETRY_BAUD 19200
#define TELEMETRY_RX_PIN 16
#define TELEMETRY_TX_PIN 17

// --- Status LEDs ---
#define LED_WIFI 4
#define LED_BLE 13
#define LED_LORA 27

// --- Link exchange / switching tuning ---
// Per-exchange timeout, shared by the Wi-Fi/BLE/LoRa PING-PONG round trips.
#define LINK_TIMEOUT_MS 300
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
