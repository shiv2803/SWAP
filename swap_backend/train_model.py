import argparse
import subprocess
from datetime import datetime, timezone
from pathlib import Path

import joblib
import pandas as pd
import sklearn
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.model_selection import train_test_split

from .common import PROTOCOL_NAMES, TelemetryRecord
from .link_quality_model import FEATURE_NAMES, compute_raw_scores, feature_means_from_records

MIN_WINDOWS_PER_CLASS_WARN = 20

REQUIRED_CSV_COLUMNS = {
    "recv_ts",
    "node",
    "ts_ms",
    "active_protocol",
    "wifi_rssi",
    "wifi_loss",
    "ble_rssi",
    "lora_rssi",
    "lora_snr",
    "lora_loss",
    "rtt_ms",
}


def _git_rev() -> str:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=5,
            check=True,
        )
        rev = result.stdout.strip()
        return rev if rev else "unversioned"
    except Exception:
        return "unversioned"


def _row_to_record(row) -> TelemetryRecord:
    return TelemetryRecord(
        recv_ts=float(row.recv_ts),
        node=str(row.node),
        ts_ms=int(row.ts_ms),
        active_protocol=int(row.active_protocol),
        wifi_rssi=float(row.wifi_rssi),
        wifi_loss=float(row.wifi_loss),
        ble_rssi=float(row.ble_rssi),
        lora_rssi=float(row.lora_rssi),
        lora_snr=float(row.lora_snr),
        lora_loss=float(row.lora_loss),
        rtt_ms=float(row.rtt_ms),
    )


def build_training_windows(df: pd.DataFrame, window: int) -> pd.DataFrame:
    """Slides a `window`-sample window per node and labels each window with
    the SAME scoring formulas used for the rule_based decision path — highest
    score wins. Feature means come from feature_means_from_records(), imported
    from link_quality_model.py rather than reimplemented here, so training and
    inference can never compute the 7 features in a different order."""
    rows = []
    for _node_id, node_df in df.groupby("node"):
        node_df = node_df.sort_values("ts_ms")
        records = [_row_to_record(r) for r in node_df.itertuples()]
        for start in range(0, len(records) - window + 1):
            window_records = records[start : start + window]
            means = feature_means_from_records(window_records)
            scores = compute_raw_scores(means)
            label_id = max(scores, key=scores.get)
            row = {name: means[name[: -len("_mean")]] for name in FEATURE_NAMES}
            row["label"] = PROTOCOL_NAMES[label_id].lower()
            rows.append(row)

    windows_df = pd.DataFrame(rows)
    if windows_df.empty:
        raise ValueError("Not enough telemetry rows to build any training windows")
    return windows_df[FEATURE_NAMES + ["label"]]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Train the SWAP link-quality RandomForest from a telemetry CSV log. "
        "Must be run on-board so the training and inference scikit-learn versions match."
    )
    parser.add_argument("--csv", required=True, help="Raw telemetry CSV log path")
    parser.add_argument(
        "--out",
        default="python/models/link_quality_rf.joblib",
        help="Output .joblib bundle path (default: %(default)s)",
    )
    parser.add_argument(
        "--window", type=int, default=10, help="Rolling window size, must match FeatureWindow (default: %(default)s)"
    )
    parser.add_argument(
        "--provenance",
        required=True,
        choices=["bootstrap-heuristic", "bench-measured", "field-measured"],
        help="Where this training data came from. bootstrap-heuristic == simulator "
        "output labelled by heuristic scoring, not real RF measurements.",
    )
    args = parser.parse_args()

    df = pd.read_csv(args.csv)
    missing_columns = REQUIRED_CSV_COLUMNS - set(df.columns)
    if missing_columns:
        raise ValueError(f"Input telemetry CSV missing columns: {sorted(missing_columns)}")

    windows_df = build_training_windows(df, window=args.window)

    class_counts = windows_df["label"].value_counts()
    print("Training window counts per class:")
    print(class_counts.to_string())
    for protocol_name in sorted(PROTOCOL_NAMES.values()):
        cls = protocol_name.lower()
        count = int(class_counts.get(cls, 0))
        if count < MIN_WINDOWS_PER_CLASS_WARN:
            print(f"WARNING: class '{cls}' has only {count} training windows (< {MIN_WINDOWS_PER_CLASS_WARN})")

    x_data = windows_df[FEATURE_NAMES]
    y_data = windows_df["label"]

    x_train, x_test, y_train, y_test = train_test_split(
        x_data, y_data, test_size=0.2, random_state=42, stratify=y_data
    )

    # n_estimators dropped 100 -> 30 (2026-08-22): sklearn's predict_proba
    # dispatches each tree through joblib's Parallel/lock-accumulation path
    # even at n_jobs=1, and on this board's CPU that per-tree overhead alone
    # was ~44ms at 100 trees. 30 keeps a real ensemble (not a single tree)
    # while cutting that cost roughly proportionally; see link_quality_model.py
    # for the custom leaf-walk inference path that removes the joblib
    # overhead entirely on top of this.
    clf = RandomForestClassifier(n_estimators=30, max_depth=8, random_state=42, class_weight="balanced")
    clf.fit(x_train, y_train)

    y_pred = clf.predict(x_test)

    print("\nClassification report:")
    print(classification_report(y_test, y_pred))

    labels = list(clf.classes_)
    cm = confusion_matrix(y_test, y_pred, labels=labels)
    print(f"Confusion matrix (rows=true, cols=predicted), label order {labels}:")
    print(cm)

    print("\nFeature importances:")
    for name, importance in sorted(zip(FEATURE_NAMES, clf.feature_importances_), key=lambda t: -t[1]):
        print(f"  {name}: {importance:.4f}")

    bundle = {
        "model": clf,
        "feature_names": FEATURE_NAMES,
        "classes": labels,
        "sklearn_version": sklearn.__version__,
        "window": args.window,
        "provenance": args.provenance,
        "n_train_rows": int(len(x_train)),
        "trained_at": datetime.now(timezone.utc).isoformat(),
        "git_rev": _git_rev(),
    }

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    joblib.dump(bundle, out_path)
    print(f"\nSaved model bundle to {out_path}")

    if args.provenance == "bootstrap-heuristic":
        print(
            "\nBOOTSTRAP MODEL WARNING: this model was trained on simulator output "
            "labelled by the same heuristic scoring functions the rule_based path "
            "already uses. It has learned to approximate those heuristics, not real "
            "RF physics. Its honest value right now is proving the full inference "
            "pipeline end to end (load, feature extraction, predict_proba, confidence "
            "gating, governor, dashboard) — not link-prediction skill. Do not present "
            "its accuracy score as evidence of link-prediction skill anywhere. Retrain "
            "with --provenance bench-measured or field-measured once real RF data "
            "exists."
        )


if __name__ == "__main__":
    main()
