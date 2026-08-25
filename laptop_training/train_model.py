import sys
from pathlib import Path

import joblib
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.model_selection import train_test_split

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))
from swap_backend.common import PROTOCOL_NAMES  # noqa: E402
from swap_backend.link_quality_model import compute_raw_scores  # noqa: E402

CSV_PATH = REPO_ROOT / "telemetry_log.csv"
MODEL_PATH = Path(__file__).parent / "swap_random_forest.joblib"
FEATURE_SEQUENCE_PATH = Path(__file__).parent / "feature_sequence.txt"

# Real telemetry schema (swap_backend/common.py CSV_FIELDS) -- matches what
# the UNO Q sketch's telemetry_line frames actually carry, no stand-ins.
FEATURE_COLS = [
    "wifi_rssi", "wifi_loss", "ble_rssi",
    "lora_rssi", "lora_snr", "lora_loss", "rtt_ms",
]
TARGET_COL = "best_protocol"


def label_rows(df: pd.DataFrame) -> pd.Series:
    """Labels each row with the SAME heuristic scoring formulas the
    rule_based decision path uses (swap_backend.link_quality_model.
    compute_raw_scores), applied per-row rather than to a rolling window
    mean -- the UNO Q app predicts one telemetry line at a time, it doesn't
    buffer a window. Reusing the canonical scoring functions instead of
    reimplementing them keeps this label definition and the backend's
    rule_based fallback from silently drifting apart."""
    labels = []
    for row in df.itertuples():
        means = {
            "wifi_rssi": row.wifi_rssi, "wifi_loss": row.wifi_loss,
            "ble_rssi": row.ble_rssi, "lora_rssi": row.lora_rssi,
            "lora_snr": row.lora_snr, "lora_loss": row.lora_loss,
            "rtt_ms": row.rtt_ms,
        }
        scores = compute_raw_scores(means)
        labels.append(max(scores, key=scores.get))
    return pd.Series(labels, index=df.index)


def main():
    print(f"Loading real telemetry from {CSV_PATH}...")
    df = pd.read_csv(CSV_PATH)

    print("Labeling rows via swap_backend's heuristic scoring formulas...")
    df[TARGET_COL] = label_rows(df)
    print("Class distribution:")
    print(df[TARGET_COL].map(PROTOCOL_NAMES).value_counts().to_string())

    X = df[FEATURE_COLS]
    y = df[TARGET_COL]

    print("\nSplitting data (80% train, 20% test, stratified)...")
    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.20, random_state=42, stratify=y
    )

    print("Training Random Forest (100 estimators, max_depth=8)...")
    rf = RandomForestClassifier(
        n_estimators=100,
        max_depth=8,
        min_samples_leaf=5,
        class_weight="balanced",
        random_state=42,
        n_jobs=-1,
    )
    rf.fit(X_train, y_train)

    print("\n--- Model Evaluation ---")
    y_pred = rf.predict(X_test)
    print("Confusion Matrix:")
    print(confusion_matrix(y_test, y_pred))
    print("\nClassification Report:")
    print(classification_report(
        y_test, y_pred,
        target_names=[PROTOCOL_NAMES[i] for i in sorted(PROTOCOL_NAMES)],
    ))

    print(f"Saving model to {MODEL_PATH}...")
    joblib.dump(rf, MODEL_PATH)

    FEATURE_SEQUENCE_PATH.write_text(",".join(FEATURE_COLS))
    print(f"Done. Model saved as {MODEL_PATH}. Feature order documented in {FEATURE_SEQUENCE_PATH}.")


if __name__ == "__main__":
    main()
