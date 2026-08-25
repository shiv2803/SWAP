"""SWAP link-quality predictor - Python (MPU) side of the UNO Q app.

Runs on the Linux side of the UNO Q. Registers a Bridge handler for
"telemetry_line" notifications pushed by sketch/sketch.ino (one JSON
telemetry frame per Node A UART line), runs the pre-trained
RandomForestClassifier on each frame, and calls the sketch's
"force_protocol" RPC with the predicted protocol.

The model (laptop_training/train_model.py) is trained on the real telemetry
schema used by swap_node / swap_backend -- wifi_rssi, wifi_loss, ble_rssi,
lora_rssi, lora_snr, lora_loss, rtt_ms -- labeled with the same heuristic
scoring formulas as swap_backend's rule_based decision path
(swap_backend/link_quality_model.py::compute_raw_scores), so the feature
order below is a direct field-for-field read of the JSON frame, no stubs.
"""

import json
from pathlib import Path

import joblib
import pandas as pd
from arduino.app_utils import App, Bridge

MODEL_DIR = Path(__file__).parent / "model"
MODEL_PATH = MODEL_DIR / "swap_random_forest.joblib"
FEATURE_SEQUENCE_PATH = MODEL_DIR / "feature_sequence.txt"

PROTOCOL_NAMES = {0: "WIFI", 1: "BLE", 2: "LORA"}

FEATURE_COLS = FEATURE_SEQUENCE_PATH.read_text().strip().split(",")
model = joblib.load(MODEL_PATH)
print(f"Loaded model from {MODEL_PATH} (features: {FEATURE_COLS})", flush=True)


def on_telemetry_line(line):
    try:
        data = json.loads(line)
    except (TypeError, ValueError) as exc:
        print(f"bad telemetry line {line!r}: {exc}", flush=True)
        return

    node = data.get("node", "a")
    try:
        row = [data[col] for col in FEATURE_COLS]
    except KeyError as exc:
        print(f"telemetry line missing {exc}: {data}", flush=True)
        return

    features = pd.DataFrame([row], columns=FEATURE_COLS)
    prediction = int(model.predict(features)[0])
    confidence = float(model.predict_proba(features)[0][prediction])
    protocol_name = PROTOCOL_NAMES.get(prediction, "UNKNOWN")

    print(
        f"node={node} predicted={protocol_name} confidence={confidence:.2f} raw={row}",
        flush=True,
    )

    try:
        Bridge.call("force_protocol", node, prediction)
    except Exception as exc:
        print(f"force_protocol failed: {exc}", flush=True)


Bridge.provide("telemetry_line", on_telemetry_line)

App.run()
