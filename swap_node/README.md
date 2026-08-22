# swap_node — ESP32 adaptive Wi-Fi/BLE/LoRa firmware

Same firmware image flashed to both ESP32 nodes; role picked at compile time
in `config.h`. Implements the adaptive Wi-Fi → BLE → LoRa switching described
in `SWAP_PROJECT_STATUS.md` §1–2 and §8 [**broken reference** — this file
does not exist anywhere in the repo; the closest candidate,
`SWAP_Project_Documentation (1).md`, uses Roman-numeral sections (I–VIII),
not this §-numeric style, so it's not a simple rename either. Left
unresolved rather than guessed], and emits telemetry matching
`swap_backend`'s `TelemetryRecord` schema over `Serial2` → CP2102 → UNO Q.

## Files

| File | Status |
|---|---|
| `config.h` | Yours, extended additively (thresholds, timeouts, `BLE_DEVICE_NAME`, `TELEMETRY_SERIAL` alias) — nothing you had was changed or removed. |
| `lora_link.h` / `.cpp` | Yours, extended: added `LoraMetrics` (RSSI/SNR/counters), the IN865 duty-cycle guard, and an interrupt-driven `receiveLoRaTelemetry()` (same `startReceive()`/DIO1-flag pattern proven in your earlier `lora_dual_sx1262_test_2.ino` bring-up test). `setupLoRa()`/`sendLoRaTelemetry()` keep your original logic, including the RXEN/TXEN fix. |
| `link_manager.h` / `.cpp` | New. `RollingMetrics`, `LinkManager`, the Wi-Fi/BLE/LoRa exchange functions, `evaluateAndSwitch()`, `emitTelemetryFrame()`, and `pollIncomingCommands()` (reads `{"cmd":"force_protocol","protocol":N}` from the UNO Q — needed for the backend's `/force` API to actually reach real hardware). |
| `swap_node.ino` | New. Thin `setup()`/`loop()` wrapper around `LinkManager`. |
| `library.properties` | **Broken — do not use.** `framework=arduino_avr` is wrong (this is an ESP32 target, not AVR), and the NimBLE download URL doesn't resolve to any real package. Install dependencies via Arduino Library Manager instead, per "Before you flash" below. |

## Before you flash

1. In `config.h`, uncomment exactly one of `NODE_ROLE_A` / `NODE_ROLE_B` per board.
2. Install libraries (Arduino Library Manager): **RadioLib** (jgromes), **NimBLE-Arduino**, **ArduinoJson**. `WiFi.h` ships with the ESP32 core. Ignore `library.properties` in this folder — see the table above.
3. Board: ESP32 Dev Module (or your specific DevKit V1 variant).

## Build status

**Compiled clean** against a real toolchain (`arduino-cli` 1.1.1, `esp32:esp32` core 3.3.11, `NimBLE-Arduino` 2.3.1, `RadioLib` 7.7.1, `ArduinoJson` 7.4.3), FQBN `esp32:esp32:esp32`:

```
Sketch uses 1203631 bytes (91%) of program storage space. Maximum is 1310720 bytes.
Global variables use 60512 bytes (18%) of dynamic memory, leaving 267168 bytes for local variables. Maximum is 327680 bytes.
```

91% flash usage on the default partition scheme is close to the ceiling — worth watching if this grows further, or if the real boards end up on a different partition table.

The NimBLE-Arduino API version drift predicted below **was hit and is fixed**: `link_manager.cpp`'s `BleServerCallbacks::onConnect`/`onDisconnect` and `BleCharCallbacks::onWrite` were written against NimBLE-Arduino 1.4.x signatures. Against the installed 2.3.1, the real signatures (verified against the installed headers, not assumed) are:
- `onConnect(NimBLEServer*, NimBLEConnInfo&)`
- `onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason)` — gained an `int reason` too, not just `NimBLEConnInfo&`
- `onWrite(NimBLECharacteristic*, NimBLEConnInfo&)`

Not yet verified on real hardware (flashing, RF behavior, actual link quality) — only that it compiles.

## Known open gap

**Node A's `ble_rssi` is left unmeasured (`-999` sentinel), deliberately.** `NimBLEServer` doesn't expose a simple per-connection RSSI getter the way `NimBLEClient::getRssi()` does on Node B's (central) side — see the comment in `tryBleExchange()`. Getting a real peripheral-side reading needs a lower-level `ble_gap_conn_rssi()` call into the NimBLE host stack; flagging as an honest open gap rather than guessing at an API that wasn't confirmed.

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
