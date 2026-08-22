# swap_node — ESP32 adaptive Wi-Fi/BLE/LoRa firmware

Same firmware image flashed to both ESP32 nodes; role picked at compile time
in `config.h`. Implements the adaptive Wi-Fi → BLE → LoRa switching described
in `SWAP_PROJECT_STATUS.md` §1–2 and §8, and emits telemetry matching
`swap_backend`'s `TelemetryRecord` schema over `Serial2` → CP2102 → UNO Q.

## Files

| File | Status |
|---|---|
| `config.h` | Yours, extended additively (thresholds, timeouts, `BLE_DEVICE_NAME`, `TELEMETRY_SERIAL` alias) — nothing you had was changed or removed. |
| `lora_link.h` / `.cpp` | Yours, extended: added `LoraMetrics` (RSSI/SNR/counters), the IN865 duty-cycle guard, and an interrupt-driven `receiveLoRaTelemetry()` (same `startReceive()`/DIO1-flag pattern proven in your earlier `lora_dual_sx1262_test_2.ino` bring-up test). `setupLoRa()`/`sendLoRaTelemetry()` keep your original logic, including the RXEN/TXEN fix. |
| `link_manager.h` / `.cpp` | New. `RollingMetrics`, `LinkManager`, the Wi-Fi/BLE/LoRa exchange functions, `evaluateAndSwitch()`, `emitTelemetryFrame()`, and `pollIncomingCommands()` (reads `{"cmd":"force_protocol","protocol":N}` from the UNO Q — needed for the backend's `/force` API to actually reach real hardware). |
| `swap_node.ino` | New. Thin `setup()`/`loop()` wrapper around `LinkManager`. |

## Before you flash

1. In `config.h`, uncomment exactly one of `NODE_ROLE_A` / `NODE_ROLE_B` per board.
2. Install libraries (Arduino Library Manager): **RadioLib** (jgromes), **NimBLE-Arduino**, **ArduinoJson**. `WiFi.h` ships with the ESP32 core.
3. Board: ESP32 Dev Module (or your specific DevKit V1 variant).

## What I could not verify

I don't have the physical boards or an ESP32 toolchain in this environment, so **none of this has been compiled**. The two areas most likely to need a fix on first build:

- **NimBLE-Arduino API version drift.** I wrote `link_manager.cpp`'s BLE code against the widely-used NimBLE-Arduino 1.4.x callback signatures (`onConnect(NimBLEServer*)`, `onWrite(NimBLECharacteristic*)`, `NimBLEScanResults`). If your installed version is 2.x, these signatures changed (added a `NimBLEConnInfo&` parameter, scan API reworked) — the compiler error will point straight at the mismatched override.
- **Node A's `ble_rssi` is left unmeasured (`-999` sentinel), deliberately.** `NimBLEServer` doesn't expose a simple per-connection RSSI getter the way `NimBLEClient::getRssi()` does on Node B's (central) side — see the comment in `tryBleExchange()`. Getting a real peripheral-side reading needs a lower-level `ble_gap_conn_rssi()` call into the NimBLE host stack; flagging as an honest open gap rather than guessing at an API I couldn't confirm.

Everything else (Wi-Fi PING/PONG over the SoftAP, LoRa PING/PONG with real RSSI/SNR, the switching state machine, telemetry framing, and command polling) follows patterns already working elsewhere in this repo (the TCP/LoRa logic mirrors your recovered `lora_dual_sx1262_test_2.ino`; the telemetry JSON schema is unit-tested indirectly by `swap_backend`'s existing `TelemetryRecord.from_json`/`train_model.py`, which already expect exactly these field names).

## Wiring to the backend

Once both nodes are flashing telemetry over their CP2102 adapters, point `swap_backend` (running directly on the UNO Q's Linux/Dragonwing side, not through Arduino App Lab) at them:

```bash
export SWAP_INPUT_MODE=serial
export SWAP_NODE_A_PORT=/dev/ttyUSB0
export SWAP_NODE_B_PORT=/dev/ttyUSB1
export SWAP_TELEMETRY_BAUD=19200
python -m uvicorn swap_backend.app:app --host 0.0.0.0 --port 8000
```

This uses `models/link_quality_model.joblib` (already in the repo) via `swap_backend/link_quality_model.py`'s rolling-window feature extraction — no separate deployment step needed on the UNO Q beyond running the backend itself.
