# SWAP — Known Limitations

Honest, evidence-based limitations of the current `swap_backend` model pipeline. Written from what was actually observed running this code, not aspirational.

## 1. Model provenance: bootstrap-heuristic

`models/link_quality_model.joblib` (as of the most recent retrain) is trained entirely on **simulator output**, labelled by the hand-tuned scoring heuristics in `link_quality_model.py` (`_score_wifi`, `_score_ble`, `_score_lora`), not on measured RF hardware.

This means the model has learned to approximate those scoring heuristics, not real RF physics. Its honest value is proving the inference pipeline end to end — feature extraction, `predict_proba`, the confidence gate, the SwitchGovernor, the dashboard — not evidence of link-prediction skill. `GET /model/info` reports `"provenance": "bootstrap-heuristic"` for exactly this reason; do not present its `classification_report` accuracy as proof the model predicts real link quality. Retrain with `--provenance bench-measured` or `--provenance field-measured` once real hardware telemetry is available.

## 2. BLE is almost never the training label — root cause identified

Across a 20-minute high-fidelity replay of the simulator (using the actual `simulator.py`/`train_model.py` code, not a re-implementation), the **windowed, score-based training label** picked BLE in only **0.42%** of windows (20 of 4,791) — versus WiFi 32.5% and LoRa 67.1%.

This is *not* the same as saying BLE is rarely usable in practice. The simulator's own instantaneous, rule-based self-report (`RuleBasedFallback`'s thresholds, the same logic `_choose_local_protocol` uses to decide each node's self-reported `active_protocol`) picks BLE **52.9%** of the time over the same period. The two mechanisms disagree by two orders of magnitude on how good BLE is.

**Root cause:** `_score_ble()` penalizes BLE using `wifi_loss`, not any BLE-specific metric:

```python
def _score_ble(m):
    return m["ble_rssi"] - (m["wifi_loss"] * 90.0) - (m["rtt_ms"] * 0.15)
```

`wifi_rssi` and `wifi_loss` move together in the simulator's phase model, so BLE only receives a small penalty exactly when WiFi is *also* healthy — and whenever WiFi is healthy, WiFi's own score wins outright. BLE is mathematically boxed out of the score-based label almost everywhere, independent of how good `ble_rssi` actually is.

**A partial fix has already been applied and verified:** `simulator.py`'s `_phase()` previously drove `ble_rssi` off the *same* sine cycle as WiFi and LoRa, making a BLE win provably impossible (0.00% across the original 43,000-window dataset). BLE's RSSI now follows an independent cycle (different period and phase offset), which moved the rate from a mathematical zero to a real, non-zero 0.42–2%. This is a genuine, verified improvement — but it treats the phase-coupling symptom, not the `wifi_loss`-coupling in the scoring formula itself, which remains the dominant cause of BLE's rarity.

**Why this wasn't fixed further in this pass:** the code comments at the scoring functions state the formula is deliberately kept numerically identical to a frontend file (`linkQualityPredictor.js`, referenced as documented in `SWAP_Project_Documentation §9`) for consistency between live and demo mode. That frontend file is outside `swap_backend/` and wasn't available to cross-check. Changing `_score_ble()` here alone would silently desync backend and frontend. This needs a decision that spans both codebases, not a unilateral backend patch.

**Upgrade path:** give BLE its own loss/penalty term (not borrowed from WiFi) in `_score_ble()`, applied identically in `swap_backend/link_quality_model.py`, `swap_backend/train_model.py`'s label function, and `linkQualityPredictor.js`, then retrain and re-verify the class distribution with the same replay method used here (`offline_ble_check.py`-style: reuse the real simulator/training code, don't hand-derive the formula).

## 3. Class imbalance and the `<20 windows` warning

`LinkQualityModel.train_and_save` warns when a class has fewer than 20 training windows. Because of limitation #2, BLE has historically had **zero** windows in the full ~43,000-window dataset — the warning never fires for an absent class, only a rare one, so this specific failure mode was previously silent. It is not silent going forward: `GET /model/info`'s `classes` field lists only the classes the model actually saw, so a missing `"ble"` entry there is the signal to check for.

## 4. UNPROVEN — not testable without hardware

- Live serial telemetry ingest (`SerialTelemetrySource`), CP2102 plug/unplug recovery, and the two-port vs. single-port topology question — no ESP32 nodes or CP2102 adapter available in this environment.
- App Lab packaging (`app.yaml` + `python/main.py` entry point) — has to be done on the physical UNO Q board.
- Real-hardware inference latency and CPU cost on the UNO Q's MPU — only measured on a Windows dev machine so far (`mean_inference_ms` via `GET /model/info`).
