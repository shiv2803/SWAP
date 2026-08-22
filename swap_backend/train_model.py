import argparse
import logging
from pathlib import Path

import pandas as pd

from .common import PROTOCOL_BLE, PROTOCOL_LORA, PROTOCOL_WIFI
from .link_quality_model import FEATURE_NAMES, MODEL_PATH, RAW_FEATURE_COLUMNS, LinkQualityModel

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("swap.train")


def _score_wifi(m: pd.Series) -> float:
    return m["wifi_rssi"] - (m["wifi_loss"] * 120.0) - (m["rtt_ms"] * 0.1)


def _score_ble(m: pd.Series) -> float:
    return m["ble_rssi"] - (m["wifi_loss"] * 90.0) - (m["rtt_ms"] * 0.15)


def _score_lora(m: pd.Series) -> float:
    return m["lora_rssi"] + (m["lora_snr"] * 2.0) - (m["lora_loss"] * 160.0) - (m["rtt_ms"] * 0.02)


def _label_best_protocol(m: pd.Series) -> int:
    scores = {
        PROTOCOL_WIFI: _score_wifi(m),
        PROTOCOL_BLE: _score_ble(m),
        PROTOCOL_LORA: _score_lora(m),
    }
    return max(scores, key=scores.get)


def build_training_csv(input_csv: Path, output_csv: Path, window: int) -> None:
    df = pd.read_csv(input_csv)
    required = set(RAW_FEATURE_COLUMNS) | {"node", "ts_ms"}
    missing = [column for column in required if column not in df.columns]
    if missing:
        raise ValueError(f"Input telemetry CSV missing columns: {missing}")

    grouped_rows = []
    for node_id, node_df in df.groupby("node"):
        node_df = node_df.sort_values("ts_ms")
        for start in range(0, len(node_df) - window + 1):
            window_df = node_df.iloc[start : start + window]
            means = window_df[RAW_FEATURE_COLUMNS].mean()
            row = {f"{col}_mean": means[col] for col in RAW_FEATURE_COLUMNS}
            row["node"] = node_id
            row["label"] = _label_best_protocol(means)
            grouped_rows.append(row)

    training_df = pd.DataFrame(grouped_rows)
    if training_df.empty:
        raise ValueError("Not enough telemetry rows to build training windows")
    training_df = training_df[FEATURE_NAMES + ["label"]]
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    training_df.to_csv(output_csv, index=False)
    logger.info("Wrote %d training windows to %s", len(training_df), output_csv)


def main() -> None:
    parser = argparse.ArgumentParser(description="Train the SWAP link-quality RandomForest model")
    parser.add_argument("--csv", default="telemetry_log.csv", help="Raw telemetry CSV log (required input)")
    parser.add_argument(
        "--training", default="models/training.csv", help="Path to write the generated windowed-feature CSV"
    )
    parser.add_argument("--out", default=str(MODEL_PATH), help="Output .joblib bundle path")
    parser.add_argument("--window", type=int, default=10, help="Rolling window size (must match FeatureWindow)")
    parser.add_argument(
        "--provenance",
        default="bootstrap-heuristic",
        choices=["bootstrap-heuristic", "bench-measured", "field-measured"],
        help="Honest label for where the training data came from",
    )
    args = parser.parse_args()

    input_csv = Path(args.csv)
    training_csv = Path(args.training)
    model_path = Path(args.out)

    if not input_csv.exists():
        raise SystemExit(f"Input telemetry CSV not found: {input_csv}")

    build_training_csv(input_csv=input_csv, output_csv=training_csv, window=args.window)
    LinkQualityModel.train_and_save(
        csv_path=str(training_csv),
        model_path=model_path,
        window=args.window,
        provenance=args.provenance,
    )


if __name__ == "__main__":
    main()
