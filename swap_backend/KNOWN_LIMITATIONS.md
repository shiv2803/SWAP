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

## 4. Control logic: two deliberate deviations from the spec, and one scoring artifact

`control.py` implements SWAP_UNO_Q_Control_Logic_Specification.md. Three things worth knowing:

**(a) The transition timeout is 5 s + 2 s grace, not a bare 5 s.** The spec sets `transitionTimeout = 5 seconds` (§10). Node A, however, spends up to ~1.5 s negotiating with Node B (retrying an `I:<N>` intent until it is acked) *before* its own 5-second trial starts, and the `switch_result` then has to travel back over the UART. Declaring failure at exactly 5.0 s would race a trial that is still legitimately running. `RESULT_GRACE_S = 2.0` covers that; the authoritative outcome is always Node A's `switch_result`, and the timeout is only a backstop for a report that never arrives.

**(b) `OPERATOR_HOLD_S = 15 s` is not in the spec at all.** The spec never mentions manual overrides. With §11's adaptive loop running autonomously, an operator's `/force` was observed being reversed by the next adaptive evaluation ~2 seconds later, which makes manual control useless. The adaptive layer now stands down for 15 s after an operator command. Remove it if the spec is ever extended to say otherwise.

**(c) The active protocol biases the scores against faster protocols.** `rtt_ms` is a single field describing whichever link is currently carrying traffic — the schema has no per-protocol RTT. But `_score_wifi` penalizes at `rtt_ms * 0.1` while `_score_lora` penalizes at `rtt_ms * 0.02`. So while LoRa is active (RTT ~200–400 ms), Wi-Fi's score absorbs a 5× larger penalty derived from a measurement that has nothing to do with Wi-Fi, and LoRa tends to keep winning. Observed directly in a simulator run: `wifi_rssi -62.9 dBm, wifi_loss 0.09` — genuinely good Wi-Fi — still scored below LoRa purely because the active LoRa link's 228 ms RTT was charged against it.

This is not a spec violation (the spec only requires Wi-Fi be the *first* protocol, and BLE be last), and it is left unchanged on purpose: the scoring formulas are shared with `train_model.py`'s label function and the frontend predictor, so a fix has to land in all three plus a retrain — the same cross-codebase decision described in §2 above. Worth deciding before claiming the adaptive layer prefers the genuinely best link rather than the incumbent one.
