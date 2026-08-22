# Archive

Superseded prototypes — kept for reference, not part of the active build.

- **`uno_q_app/`** (what remains of it) and **`laptop_training/`** use a different 7-feature schema (`wifi_rssi, ble_rssi, lora_rssi, lora_snr, pdr, rtt, retries`) than the current `swap_backend/` (`*_loss` fields, no `pdr`/`retries`). Wiring either of these to the live backend would silently corrupt inference — the feature order and meaning don't match.
- `swap_backend/` is the canonical implementation going forward, including the model training/deployment pipeline.

**Correction:** `uno_q_app/sketch/sketch.ino` was originally archived here along with the rest of `uno_q_app/`, but it was never actually stale — it's a real, current "Node A UART forwarder" for the UNO Q's own microcontroller, unrelated to the old `pdr`/`rtt`/`retries` schema issue above (that issue is specific to `uno_q_app/python/main.py`). It's been moved out to `../uno_q_mcu_sketch/`. Only `uno_q_app/python/` and the leftover `sketch/sketch.yaml.bak` remain here, both genuinely stale (unchanged since 2026-08-21, before any of the current `swap_backend`/`swap_node` work existed).
