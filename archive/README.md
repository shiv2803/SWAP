# Archive

Currently empty. Kept as the designated home for genuinely superseded
prototypes — not part of the active build — should anything need to move
here in future.

**`laptop_training/` and `uno_q_app/` were both restored to the repo root at
the project owner's explicit request** and are no longer archived. Both use a
different 7-feature schema (`wifi_rssi, ble_rssi, lora_rssi, lora_snr, pdr,
rtt, retries`) than the current `swap_backend/` (`*_loss` fields, no
`pdr`/`retries`) — wiring either to the live backend would silently corrupt
inference, the feature order and meaning don't match — but the owner wants
them kept in place regardless, so treat them as intentionally-kept dead code,
not leftovers to re-sweep in here. `swap_backend/` remains the canonical
implementation, including the model training/deployment pipeline.

`uno_q_app/sketch/sketch.ino` was never part of either — that file was
correctly identified as non-stale early on and lives in
`../uno_q_mcu_sketch/` (a real, current "Node A UART forwarder" for the UNO
Q's own microcontroller).
