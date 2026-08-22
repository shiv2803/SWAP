# Archive

Both folders here are earlier, superseded prototypes — kept for reference, not part of the active build.

- **`uno_q_app/`** and **`laptop_training/`** use a different 7-feature schema (`wifi_rssi, ble_rssi, lora_rssi, lora_snr, pdr, rtt, retries`) than the current `swap_backend/` (`*_loss` fields, no `pdr`/`retries`). Wiring either of these to the live backend would silently corrupt inference — the feature order and meaning don't match.
- `swap_backend/` is the canonical implementation going forward, including the model training/deployment pipeline.
