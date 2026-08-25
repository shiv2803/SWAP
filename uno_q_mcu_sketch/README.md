# uno_q_mcu_sketch

`sketch.ino` here runs on the UNO Q's **own microcontroller** (the Zephyr-based STM32 side) — not on the ESP32 Node A/B boards (`swap_node/`), and not part of `swap_backend/` (the Linux/Dragonwing Python side).

It's a Node A UART forwarder: reads newline-delimited JSON telemetry from Node A on `Serial2` (D14/D15, 115200 8N1 — moved off `Serial1`/D0-D1 because that UART is also the Zephyr console, which interleaved its own output into the telemetry and produced shredded frames like `{"li{"li{`), forwards each line to the UNO Q's Python side over the Router Bridge (`Bridge.notify("telemetry_line", ...)`), and relays `force_protocol` commands back down to Node A. Line buffering has a hard length cap so a stuck-high line can't grow the buffer unbounded.

**Why it's not in `swap_node/`:** it only builds inside Arduino App Lab's own Zephyr toolchain (`Arduino_RouterBridge.h` requires `zephyr/kernel.h`, which doesn't exist outside that build environment) — mixing it into the ESP32 sketch folder breaks compilation for both.

**Where this needs to end up:** per the App Lab primer's Task 2 layout, this belongs in `~/ArduinoApps/swap-edge-ai/sketch/sketch.ino` once that App Lab project exists (Task 1, still pending — needs the board). Staged here at the repo root in the meantime so it isn't lost or miscategorized as stale again.

A copy also now lives at `../uno_q_app/sketch/sketch.ino`, since that's the local App Lab project folder that pairs with it (`../uno_q_app/python/main.py` is written against this sketch's `telemetry_line`/`force_protocol` Bridge calls). This file here stays the source of truth — keep both in sync if you edit it.
