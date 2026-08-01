import argparse
from pathlib import Path

import pandas as pd

from .common import PROTOCOL_BLE, PROTOCOL_LORA, PROTOCOL_WIFI
from .link_quality_model import FEATURE_NAMES, LinkQualityModel


def _score_wifi(row: pd.Series) -> float:
    return row["wifi_rssi"] - (row["wifi_loss"] * 120.0) - (row["rtt_ms"] * 0.1)


def _score_ble(row: pd.Series) -> float:
    return row["ble_rssi"] - (row["wifi_loss"] * 90.0) - (row["rtt_ms"] * 0.15)


def _score_lora(row: pd.Series) -> float:
    return row["lora_rssi"] + (row["lora_snr"] * 2.0) - (row["lora_loss"] * 160.0) - (row["rtt_ms"] * 0.02)


def _label_best_protocol(row: pd.Series) -> int:
    scores = {
        PROTOCOL_WIFI: _score_wifi(row),
        PROTOCOL_BLE: _score_ble(row),
        PROTOCOL_LORA: _score_lora(row),
    }
    return max(scores, key=scores.get)


def build_training_csv(input_csv: Path, output_csv: Path) -> None:
    df = pd.read_csv(input_csv)
    expected = {"node", "wifi_rssi", "wifi_loss", "ble_rssi", "lora_rssi", "lora_snr", "lora_loss", "rtt_ms"}
    missing = [column for column in expected if column not in df.columns]
    if missing:
        raise ValueError(f"Input telemetry CSV missing columns: {missing}")

    grouped_rows = []
    for node_id, node_df in df.groupby("node"):
        for start in range(0, len(node_df) - 9):
            window_df = node_df.iloc[start : start + 10]
            row = {
                "node": node_id,
                "wifi_rssi_mean": window_df["wifi_rssi"].mean(),
                "wifi_loss_mean": window_df["wifi_loss"].mean(),
                "ble_rssi_mean": window_df["ble_rssi"].mean(),
                "lora_rssi_mean": window_df["lora_rssi"].mean(),
                "lora_snr_mean": window_df["lora_snr"].mean(),
                "lora_loss_mean": window_df["lora_loss"].mean(),
                "rtt_ms_mean": window_df["rtt_ms"].mean(),
            }
            row["label"] = _label_best_protocol(pd.Series({
                "wifi_rssi": row["wifi_rssi_mean"],
                "wifi_loss": row["wifi_loss_mean"],
                "ble_rssi": row["ble_rssi_mean"],
                "lora_rssi": row["lora_rssi_mean"],
                "lora_snr": row["lora_snr_mean"],
                "lora_loss": row["lora_loss_mean"],
                "rtt_ms": row["rtt_ms_mean"],
            }))
            grouped_rows.append(row)

    training_df = pd.DataFrame(grouped_rows)
    if training_df.empty:
        raise ValueError("Not enough telemetry rows to build training windows")
    training_df = training_df[FEATURE_NAMES + ["label"]]
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    training_df.to_csv(output_csv, index=False)


def main() -> None:
    parser = argparse.ArgumentParser(description="Train SWAP prototype model from telemetry log")
    parser.add_argument("--input", default="telemetry_log.csv", help="Telemetry CSV path")
    parser.add_argument("--training", default="models/training.csv", help="Generated training CSV path")
    parser.add_argument("--model", default="models/link_quality_model.joblib", help="Output model path")
    args = parser.parse_args()

    input_csv = Path(args.input)
    training_csv = Path(args.training)
    model_path = Path(args.model)

    build_training_csv(input_csv=input_csv, output_csv=training_csv)
    LinkQualityModel.train_and_save(csv_path=str(training_csv), model_path=model_path)


if __name__ == "__main__":
    main()

