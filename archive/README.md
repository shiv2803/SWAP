# Archive

Currently empty. Kept as the designated home for genuinely superseded
prototypes — not part of the active build — should anything need to move
here in future.

**`laptop_training/` and `uno_q_app/` were both restored to the repo root at
the project owner's explicit request** and are no longer archived.

They used to carry a different 7-feature schema (`wifi_rssi, ble_rssi,
lora_rssi, lora_snr, pdr, rtt, retries`) than `swap_backend/`'s real one
(`*_loss` fields, no `pdr`/`retries`), which would have silently corrupted
inference if wired to the live backend. **Fixed 2026-08-23:**
`laptop_training/train_model.py` now trains on the real schema
(`wifi_rssi, wifi_loss, ble_rssi, lora_rssi, lora_snr, lora_loss, rtt_ms`)
against real recorded telemetry (`telemetry_log.csv`), with labels from
`swap_backend.link_quality_model.compute_raw_scores` — the same heuristic
formulas the backend's own rule-based path uses. `uno_q_app/python/main.py`
was updated to match. `swap_backend/` remains the canonical implementation,
including the model training/deployment pipeline; the UNO Q app's model is a
separate, per-line (non-windowed) classifier trained on the same features
and label formulas.

`uno_q_app/sketch/sketch.ino` was never part of either — that file was
correctly identified as non-stale early on and lives in
`../uno_q_mcu_sketch/` (a real, current "Node A UART forwarder" for the UNO
Q's own microcontroller).
