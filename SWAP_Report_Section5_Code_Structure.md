# SWAP — Arduino Challenge Report, Section 5: Code Structure

> Copy-paste ready. Every function named below is a real symbol in the
> repository, not an illustrative example.

---

## Overview — three tiers

SWAP runs across three processors, and the code is organised to match:

```
Node A / Node B  (ESP32)         →  radio link + link-quality measurement
     │ UART D0/D1, 115200 8N1, newline-delimited JSON
Arduino UNO Q    (STM32 MCU)     →  UART ⇄ Bridge relay
     │ Arduino Router Bridge
Arduino UNO Q    (Linux SoC)     →  ML inference + protocol decisions
```

Both nodes flash **identical firmware**; role is compile-time only, via
`#define NODE_ROLE_A` in `config.h`.

---

## 5.1 ESP32 node firmware — `swap_node/`

| Function | Role |
|---|---|
| `setup()` | Starts serial, prints the role banner, calls `linkManager.begin()`. |
| `loop()` | One line — delegates to `linkManager.loop()`. All logic lives in the class. |
| `LinkManager::begin()` | Brings up Wi-Fi, BLE and LoRa radios; calls `setupLoRa()`. |
| `LinkManager::loop()` | Main cycle: runs an exchange on the active protocol, polls for commands, evaluates whether to switch, emits telemetry. |
| `LinkManager::tryWifiExchange()` | One Wi-Fi PING/PONG round trip over TCP; records RSSI, RTT, success. |
| `LinkManager::tryBleExchange()` | Same over BLE (NimBLE notify/write characteristic). |
| `LinkManager::tryLoraExchange()` | Same over LoRa (SX1262); records RSSI, SNR, RTT. |
| `LinkManager::evaluateAndSwitch()` | The switching state machine — compares rolling metrics against thresholds and selects the active protocol. Honours `forcedProtocol_` when the UNO Q overrides it. |
| `LinkManager::emitTelemetryFrame()` | Emits one JSON telemetry line upstream over `TELEMETRY_SERIAL`. |
| `LinkManager::pollIncomingCommands()` | Reads `{"cmd":"force_protocol","protocol":N}` from the UNO Q and applies the override. |
| `LinkManager::buildOwnPeerPayload()` | **Node B:** packs its own metrics as CSV, piggybacked on the wireless reply instead of a bare `PONG`. |
| `LinkManager::parsePeerPayload()` / `emitNodeBTelemetryFrame()` | **Node A:** decodes Node B's relayed metrics and forwards them upstream as a second `"node":"b"` frame — so the UNO Q sees both ends of the link. |

**`RollingMetrics` class** — fixed circular buffer over the last
`METRIC_WINDOW_SAMPLES` (10) exchanges per protocol. Exposes `push()`,
`avgRssi()`, `avgRtt()`, `packetLossRate()`, `sampleCount()`. This is what both
the switching logic and the telemetry frame read from, so a one-off bad packet
cannot trigger a handover on its own.

### Radio driver — `swap_node/lora_link.cpp`

| Function | Role |
|---|---|
| `setupLoRa()` | Initialises the SX1262. Sets RXEN/TXEN explicitly via `setRfSwitchPins()` — the Core1262-HF has no internal RF-switch wiring on DIO2, so these **must** be driven in firmware. Passes TCXO voltage (1.6 V) and preamble length explicitly rather than trusting library defaults. |
| `sendLoRaTelemetry()` | Transmits a payload after checking the IN865 duty-cycle budget; refuses to transmit if it would breach the 1% limit. |
| `receiveLoRaTelemetry()` | Interrupt-driven receive with timeout. Returns RSSI/SNR on success; forces the radio back to `standby()` on **every** failure path. |
| `estimateAirTimeMs()` | Semtech air-time estimate used only for duty-cycle budgeting. |
| `onLoraDio1()` | ISR — sets a flag only; never touches Serial/SPI/RadioLib. |

---

## 5.2 UNO Q MCU sketch — `uno_q_app/sketch/sketch.ino`

A deliberately thin relay: it does **not** parse JSON, only reassembles lines.

| Function | Role |
|---|---|
| `setup()` | Starts the Router Bridge, registers `force_protocol` and `uart_send_raw` RPCs, opens `Serial1` at 115200. |
| `loop()` | Reads Node A byte-by-byte, reassembles newline-delimited lines, pushes each complete line to Python as a `telemetry_line` notification. Enforces a 2048-byte cap so a stuck-high line cannot grow the buffer without bound. |
| `forceProtocol(node, protocol)` | Called *from* Python; writes the JSON command back down the UART to Node A. |
| `uartSendRaw(message)` | Raw passthrough for probing the physical link directly, bypassing the JSON envelope. |

> **Why 115200, not 9600:** `usart1` (D0/D1) is also the Zephyr console UART and
> holds the line at its own 115200 default regardless of what `Serial1.begin()`
> requests. Running 9600 produced a baud mismatch and near-zero valid frames.

---

## 5.3 UNO Q Linux app — `uno_q_app/python/main.py`

| Function | Role |
|---|---|
| *module load* | Reads `model/feature_sequence.txt` to fix feature order, loads `swap_random_forest.joblib`. Feature order comes from the file, never hard-coded — so it cannot drift from the trained model. |
| `on_telemetry_line(line)` | Parses one JSON frame, builds the feature row, runs `model.predict()` + `predict_proba()`, logs `node=… predicted=… confidence=…`, then calls the sketch's `force_protocol` RPC with the result. |
| `Bridge.provide("telemetry_line", …)` | Registers the handler the sketch notifies. |
| `App.run()` | Enters the App Lab event loop. |

---

## 5.4 Supporting components

| Path | Role |
|---|---|
| `swap_backend/link_quality_model.py` | `compute_raw_scores()` — canonical heuristic scoring, used both by the rule-based fallback **and** to label training data, so the two cannot silently diverge. |
| `swap_backend/simulator.py` | Generates synthetic link conditions for testing without hardware. |
| `swap_backend/app.py`, `telemetry_link.py` | Dashboard backend and telemetry ingestion. |
| `laptop_training/train_model.py` | Offline training — labels rows via `compute_raw_scores()`, trains the Random Forest, writes both the model and `feature_sequence.txt`. |
| `swap-frontend/` | Web dashboard. |
| `lora_bringup_a/`, `lora_bringup_b/` | Minimal two-node LoRa ping/ack test — no Wi-Fi, BLE or switching. Used to prove the physical link before layering the adaptive stack on top. |
| `lora_spi_diag/`, `pin_integrity/`, `hello_serial/` | Hardware bring-up diagnostics: raw SPI/BUSY probe (no RadioLib), GPIO integrity test, and a serial sanity check. |

---

## 5.5 Data flow, end to end

```
1. Node B measures its link quality        → buildOwnPeerPayload()  [CSV]
2. Node A exchanges + receives B's metrics → parsePeerPayload()
3. Node A emits two JSON frames (a, b)     → emitTelemetryFrame()
                                              emitNodeBTelemetryFrame()
4. UNO Q sketch reassembles lines          → Bridge.notify("telemetry_line")
5. Python runs the classifier              → on_telemetry_line()
6. Decision returns to Node A              → force_protocol → pollIncomingCommands()
7. Node A applies the override             → evaluateAndSwitch()
```

---

## GitHub Repository field

```
https://github.com/shiv2803/SWAP
```
