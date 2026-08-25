# SWAP — Next Steps (TAG-Framework Prompts)

Roadmap for picking hardware bring-up back up after `lora_bringup_a`/`lora_bringup_b`
(minimal Node A → Node B LoRa link + OLED status, no WiFi/BLE/adaptive switching yet).

Each item uses the **TAG framework**: **T**ask (what needs doing), **A**ction
(concrete steps), **G**oal (how you know it's done). Use these as prompts,
in order — each depends on the previous one actually working.

---

## 1. Two-way LoRa ack

- **Task:** Add a pong/ack reply from Node B back to Node A.
- **Action:** In `lora_bringup_b.ino`, after a successful `receiveLoRaTelemetry()`,
  call `sendLoRaTelemetry("PONG <seq>")` back. In `lora_bringup_a.ino`, after
  each `sendLoRaTelemetry()`, call `receiveLoRaTelemetry()` with a short
  timeout to catch the reply.
- **Goal:** Both OLEDs show round-trip time (ms) per exchange, not just
  one-directional counts.

## 2. Fix the UART baud mismatch

- **Task:** Resolve the 19200 vs 115200 baud conflict between Node A and the
  UNO Q sketch.
- **Action:** Change `TELEMETRY_BAUD` in `swap_node/config.h` (and
  `swap_node_a/config.h`) from `19200` to `115200`, since the UNO Q's
  USART1 can't be moved off 115200 (it's also the Zephyr console UART).
- **Goal:** Real telemetry frames from Node A are legible on the UNO Q's
  `Serial1`, confirmed by non-garbage JSON in the sketch's Monitor log.

## 3. Merge into full adaptive switching

- **Task:** Bring the proven two-way LoRa link into `swap_node_a`/
  `swap_node_b`'s full WiFi/BLE/LoRa adaptive logic.
- **Action:** Validate `lora_link.cpp` usage inside `link_manager.cpp`
  against the bring-up test's confirmed behavior, then flash both boards
  and run them simultaneously for the first time (needs two USB
  cables/power sources — never done before).
- **Goal:** Node A and B run the full adaptive stack concurrently without
  crashing, with the switching state machine observed making real
  handover decisions.

## 4. Wire Node A into the UNO Q pipeline

- **Task:** Get real hardware telemetry flowing into
  `uno_q_app/python/main.py`, not just synthetic dry-run data.
- **Action:** Connect Node A's `TELEMETRY_SERIAL` (Serial2, pins 16/17) to
  the UNO Q's sketch UART (D0/D1), flash `uno_q_mcu_sketch`/
  `uno_q_app/sketch`, and watch the App Lab Monitor for `telemetry_line`
  notifications with real values.
- **Goal:** `main.py` logs `node=a predicted=... raw=[...]` with real
  RSSI/loss/RTT numbers, and `force_protocol` calls reach Node A.

## 5. Validate/retrain on real data

- **Task:** Decide if the model needs retraining on real (not simulator)
  telemetry.
- **Action:** Once #4 is flowing, log a batch of real hardware telemetry,
  compare model predictions against expected protocol choices, and rerun
  `laptop_training/train_model.py` against the real-hardware CSV if
  predictions look off.
- **Goal:** Model accuracy/behavior validated against physically-measured
  RF conditions, not just the backend's simulator.

---

## Status snapshot (as of 2026-08-23)

| Item | State |
|---|---|
| UNO Q App Lab app (sketch + Python + model) | Done — retrained on real telemetry schema, running |
| `lora_bringup_a`/`lora_bringup_b` | Written, compile-checked, not yet flashed to real hardware |
| Items 1–5 above | Not started |
