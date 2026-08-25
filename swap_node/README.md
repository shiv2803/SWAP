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

**Flashed and running on real ESP32 DevKit V1 hardware** (not just compiled). FQBN `esp32:esp32:esp32`, `esp32:esp32` core 3.3.11:

```
Sketch uses 1203631 bytes (91%) of program storage space. Maximum is 1310720 bytes.
Global variables use 60512 bytes (18%) of dynamic memory, leaving 267168 bytes for local variables. Maximum is 327680 bytes.
```

91% flash usage on the default partition scheme is close to the ceiling — worth watching if this grows further, or if the real boards end up on a different partition table.

The NimBLE-Arduino API version drift predicted below **was hit and is fixed**: `link_manager.cpp`'s `BleServerCallbacks::onConnect`/`onDisconnect` and `BleCharCallbacks::onWrite` were written against NimBLE-Arduino 1.4.x signatures. Against 2.3.1, the real signatures (verified against the installed headers, not assumed) are:
- `onConnect(NimBLEServer*, NimBLEConnInfo&)`
- `onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason)` — gained an `int reason` too, not just `NimBLEConnInfo&`
- `onWrite(NimBLECharacteristic*, NimBLEConnInfo&)`

**A second, more serious NimBLE issue surfaced only on real hardware** (2026-08-23): with the signatures above fixed and compiling clean, the board crash-looped every boot with `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)`, `EXCVADDR: 0x00000044` — a null-pointer dereference. Decoded the actual crash backtrace against the compiled `.elf` with `xtensa-esp32-elf-addr2line` rather than guess: `LinkManager::beginBle()` (`link_manager.cpp:94`, `NimBLEDevice::createServer()`) → NimBLE's internal `ble_gatts_reset()` → `ble_hs_lock()` → `ble_npl_mutex_pend()` on what looked like an uninitialized mutex, every time, regardless of free heap (confirmed 176KB+ free at the crash site, ruling out WiFi+BT heap exhaustion as the cause). Root cause: a version-compatibility bug in **NimBLE-Arduino 2.3.1** against `arduino-esp32` core 3.3.11 — this combination has multiple documented crash reports upstream (h2zero/NimBLE-Arduino issues #676, #688, #641). **Fixed by upgrading to NimBLE-Arduino 2.5.1** — confirmed on real hardware:
```
[Heap] free=138884 bytes before beginBle()
[BLE] advertising as peripheral (Node A)
[Heap] free=117404 bytes after beginBle()
```
No crash, BLE advertises correctly as a peripheral. If you're setting this up fresh, install NimBLE-Arduino 2.5.1 (or later) directly — don't start from 2.3.1.

**LoRa `Failed, code -2` (`RADIOLIB_ERR_CHIP_NOT_FOUND`) at boot is expected on a bare DevKit V1 with no SX1262 module wired up** — not a firmware bug. Every subsequent `[LoRa] Transmit failed` / `[LoRa] startReceive failed` is the same underlying cause repeating. The firmware correctly falls back to LoRa as `evaluateAndSwitch()`'s last resort when WiFi/BLE don't have enough samples yet (`[LinkManager] switching protocol: 0 -> 2` at boot) — that's the state machine working as intended, not malfunctioning. Wire up a real SX1262 module matching `config.h`'s pin map (still unverified against physical wiring) to actually exercise the LoRa path.

**Node B's role also hit a real, separate NimBLE 2.x API break — fixed and confirmed on real hardware (2026-08-23).** Node B's code path (`beginBle()`'s `#else` branch, BLE central/scan) had never been compiled before this point (only `NODE_ROLE_A` had been built and flashed so far). Compiling with `NODE_ROLE_A` commented out surfaced two more 1.x-vs-2.x signature breaks in `link_manager.cpp`'s scan logic:
- `NimBLEScan::start(duration, isContinue)` now returns `bool` (did the scan start), not the results — `NimBLEScan::getResults(duration, isContinue)` is the direct replacement for the old "start and block until done" pattern.
- `NimBLEScanResults::getDevice(idx)` now returns `const NimBLEAdvertisedDevice*`, not a value — and `NimBLEClient::connect()` takes that pointer directly, so the old `connect(&device)` became a pointer-to-pointer bug once `device` itself became a pointer.

Both fixed and flashed to real hardware (same board, reflashed — only one USB cable available, so Node A and Node B have not yet been tested simultaneously). Confirmed clean, no crash:
```
[WiFi] STA connect timed out — will retry lazily during switching
[BLE] scanning for Node A...
[BLE] Node A not found this scan — will retry lazily during switching
[LinkManager] switching protocol: 0 -> 2
```
Both the WiFi-station-timeout and BLE-scan-found-nothing paths are expected here since no Node A is currently powered — this confirms Node B's code doesn't crash when run standalone, not that two-node communication has been tested. That still needs a second USB cable/board powered simultaneously with this one.

## Known open gap

**Node A's `ble_rssi` is left unmeasured (`-999` sentinel), deliberately.** `NimBLEServer` doesn't expose a simple per-connection RSSI getter the way `NimBLEClient::getRssi()` does on Node B's (central) side — see the comment in `tryBleExchange()`. Getting a real peripheral-side reading needs a lower-level `ble_gap_conn_rssi()` call into the NimBLE host stack; flagging as an honest open gap rather than guessing at an API that wasn't confirmed.

Everything else (Wi-Fi PING/PONG over the SoftAP, LoRa PING/PONG with real RSSI/SNR, the switching state machine, telemetry framing, and command polling) follows patterns already working elsewhere in this repo (the TCP/LoRa logic mirrors your recovered `lora_dual_sx1262_test_2.ino`; the telemetry JSON schema is unit-tested indirectly by `swap_backend`'s existing `TelemetryRecord.from_json`/`train_model.py`, which already expect exactly these field names).

## Wiring to the backend

**Correction (2026-08-23):** this section previously described `swap_backend` opening `/dev/ttyUSB0`/`/dev/ttyUSB1` directly via `SWAP_NODE_A_PORT`/`SWAP_NODE_B_PORT`. That's wrong and no longer how it works — Python opening the MCU-side UART directly violates the project's own hard constraint (that interface belongs exclusively to the UNO Q's own microcontroller / `arduino-router` service). The actual, correct path: Node A's `TELEMETRY_SERIAL` (`Serial2`, pins 16/17 here) goes to the UNO Q's own microcontroller, which runs `uno_q_mcu_sketch/sketch.ino` (a UART forwarder — reads Node A's lines on its `Serial1`, relays each one to Python over the Router Bridge via `Bridge.notify("telemetry_line", ...)`). `swap_backend/telemetry_link.py`'s `BridgeTelemetrySource` receives them on the Python side — no raw serial device is opened from Python at all.

```bash
export SWAP_INPUT_MODE=serial
python -m uvicorn swap_backend.app:app --host 0.0.0.0 --port 8000
```

This uses `models/link_quality_rf.joblib` (already in the repo) via `swap_backend/link_quality_model.py`'s rolling-window feature extraction — no separate deployment step needed on the UNO Q beyond running the backend itself and having `uno_q_mcu_sketch/sketch.ino` flashed to its microcontroller.
