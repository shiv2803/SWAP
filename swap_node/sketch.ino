#include "Arduino_RouterBridge.h"
#include <ArxContainer.h>

// Demo indicator LEDs — wire one LED (with resistor) per pin, or just watch
// the Serial Monitor output. Change pins to match your wiring.
#define WIFI_LED 5
#define BLE_LED 6
#define LORA_LED 7

using Telemetry = std::array<float, 7>;

// Returns the 7 link-quality readings the Python side feeds into the
// RandomForestClassifier, in the exact order recorded in
// laptop_training/feature_sequence.txt:
//   wifi_rssi, ble_rssi, lora_rssi, lora_snr, pdr, rtt, retries
//
// TODO: replace these placeholder values with real reads from your Wi-Fi,
// BLE, and LoRa radios (e.g. WiFi.RSSI(), a BLE scan RSSI, your LoRa module's
// RSSI/SNR registers, and PDR/RTT/retry counters from your link layer).
Telemetry get_telemetry() {
  Telemetry t;
  t[0] = -65.0f;  // wifi_rssi (dBm)
  t[1] = -70.0f;  // ble_rssi (dBm)
  t[2] = -95.0f;  // lora_rssi (dBm)
  t[3] = 6.0f;    // lora_snr (dB)
  t[4] = 0.98f;   // pdr (0-1)
  t[5] = 25.0f;   // rtt (ms)
  t[6] = 0.0f;    // retries
  return t;
}

// Called by the Python side after each prediction. 0 = WIFI, 1 = BLE, 2 = LORA.
void set_protocol(int protocol) {
  digitalWrite(WIFI_LED, protocol == 0 ? HIGH : LOW);
  digitalWrite(BLE_LED, protocol == 1 ? HIGH : LOW);
  digitalWrite(LORA_LED, protocol == 2 ? HIGH : LOW);
  Monitor.print("set_protocol called: ");
  Monitor.println(protocol);
}

void setup() {
  Bridge.begin();
  Monitor.begin();

  pinMode(WIFI_LED, OUTPUT);
  pinMode(BLE_LED, OUTPUT);
  pinMode(LORA_LED, OUTPUT);

  Bridge.provide_safe("get_telemetry", get_telemetry);
  Bridge.provide_safe("set_protocol", set_protocol);

  Monitor.println("Bridge ready");
}

void loop() {}
