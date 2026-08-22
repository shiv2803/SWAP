"""SWAP link-quality predictor - Python (MPU) side of the UNO Q app.

Loads the pre-trained RandomForestClassifier (models/link_quality_model.joblib
in laptop_training/, trained by laptop_training/train_model.py) and, once per
cycle, asks the sketch (MCU) for the latest link telemetry, runs inference,
and tells the sketch which protocol to indicate as active.
"""

import time
from pathlib import Path

import joblib
import pandas as pd
from arduino.app_utils import App, Bridge, Monitor

MODEL_DIR = Path(__file__).parent / "model"
MODEL_PATH = MODEL_DIR / "swap_random_forest.joblib"
FEATURE_SEQUENCE_PATH = MODEL_DIR / "feature_sequence.txt"

PROTOCOL_NAMES = {0: "WIFI", 1: "BLE", 2: "LORA"}
PREDICT_INTERVAL_S = 1.0

FEATURE_COLS = FEATURE_SEQUENCE_PATH.read_text().strip().split(",")
model = joblib.load(MODEL_PATH)
Monitor.println(f"Loaded model from {MODEL_PATH} (features: {FEATURE_COLS})")


def loop():
    try:
        # Sketch must expose get_telemetry() returning the 7 readings below,
        # in FEATURE_COLS order: wifi_rssi, ble_rssi, lora_rssi, lora_snr,
        # pdr, rtt, retries. See sketch/sketch.ino.
        readings = Bridge.call("get_telemetry")
    except Exception as exc:
        Monitor.println(f"get_telemetry failed: {exc}")
        time.sleep(PREDICT_INTERVAL_S)
        return

    features = pd.DataFrame([readings], columns=FEATURE_COLS)
    prediction = int(model.predict(features)[0])
    confidence = float(model.predict_proba(features)[0][prediction])
    protocol_name = PROTOCOL_NAMES.get(prediction, "UNKNOWN")

    Monitor.println(
        f"predicted={protocol_name} confidence={confidence:.2f} raw={readings}"
    )

    try:
        Bridge.call("set_protocol", prediction)
    except Exception as exc:
        Monitor.println(f"set_protocol failed: {exc}")

    time.sleep(PREDICT_INTERVAL_S)


App.run(user_loop=loop)
