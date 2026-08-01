import logging
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Deque, Dict, List, Optional

import joblib
import numpy as np

from .common import PROTOCOL_BLE, PROTOCOL_LORA, PROTOCOL_WIFI, TelemetryRecord

logger = logging.getLogger("swap.model")

FEATURE_NAMES = [
    "wifi_rssi_mean",
    "wifi_loss_mean",
    "ble_rssi_mean",
    "lora_rssi_mean",
    "lora_snr_mean",
    "lora_loss_mean",
    "rtt_ms_mean",
]

MODEL_PATH = Path("models") / "link_quality_model.joblib"


@dataclass
class Decision:
    protocol: int
    confidence: float
    source: str


class FeatureWindow:
    def __init__(self, window_size: int):
        self._window: Deque[TelemetryRecord] = deque(maxlen=window_size)

    def push(self, record: TelemetryRecord) -> None:
        self._window.append(record)

    def is_ready(self, min_samples: int) -> bool:
        return len(self._window) >= min_samples

    def to_feature_vector(self) -> np.ndarray:
        arr = np.array(
            [
                [
                    r.wifi_rssi,
                    r.wifi_loss,
                    r.ble_rssi,
                    r.lora_rssi,
                    r.lora_snr,
                    r.lora_loss,
                    r.rtt_ms,
                ]
                for r in self._window
            ]
        )
        means = arr.mean(axis=0)
        return means.reshape(1, -1)


class RuleBasedFallback:
    WIFI_RSSI_MIN = -75.0
    WIFI_LOSS_MAX = 0.10
    BLE_RSSI_MIN = -85.0

    def decide(self, features: np.ndarray) -> Decision:
        wifi_rssi, wifi_loss, ble_rssi, _, _, _, _ = features.flatten()
        if wifi_rssi > self.WIFI_RSSI_MIN and wifi_loss < self.WIFI_LOSS_MAX:
            return Decision(protocol=PROTOCOL_WIFI, confidence=0.60, source="rule_based")
        if ble_rssi > self.BLE_RSSI_MIN:
            return Decision(protocol=PROTOCOL_BLE, confidence=0.50, source="rule_based")
        return Decision(protocol=PROTOCOL_LORA, confidence=0.40, source="rule_based")


class LinkQualityModel:
    def __init__(
        self,
        model_path: Path = MODEL_PATH,
        window_size: int = 10,
        min_samples: int = 3,
    ):
        self._fallback = RuleBasedFallback()
        self._model = self._try_load(model_path)
        self._window_size = window_size
        self._min_samples = min_samples
        self._windows: Dict[str, FeatureWindow] = {}

    @staticmethod
    def _try_load(model_path: Path):
        try:
            model = joblib.load(model_path)
            logger.info("Loaded trained model from %s", model_path)
            return model
        except FileNotFoundError:
            logger.info("No trained model found at %s, using fallback", model_path)
            return None

    def _window_for(self, node_id: str) -> FeatureWindow:
        if node_id not in self._windows:
            self._windows[node_id] = FeatureWindow(window_size=self._window_size)
        return self._windows[node_id]

    def observe(self, record: TelemetryRecord) -> Optional[Decision]:
        window = self._window_for(record.node)
        window.push(record)
        if not window.is_ready(self._min_samples):
            return None
        return self._predict_from_window(window)

    def recommend_for_node(self, node_id: str) -> Optional[Decision]:
        window = self._windows.get(node_id)
        if window is None or not window.is_ready(self._min_samples):
            return None
        return self._predict_from_window(window)

    def _predict_from_window(self, window: FeatureWindow) -> Decision:
        features = window.to_feature_vector()
        if self._model is not None:
            pred = int(self._model.predict(features)[0])
            proba = self._model.predict_proba(features)[0]
            return Decision(protocol=pred, confidence=float(np.max(proba)), source="model")
        return self._fallback.decide(features)

    @staticmethod
    def train_and_save(
        csv_path: str,
        model_path: Path = MODEL_PATH,
    ) -> None:
        import pandas as pd
        from sklearn.ensemble import RandomForestClassifier
        from sklearn.metrics import classification_report
        from sklearn.model_selection import train_test_split

        df = pd.read_csv(csv_path)
        required_columns: List[str] = FEATURE_NAMES + ["label"]
        missing = [column for column in required_columns if column not in df.columns]
        if missing:
            raise ValueError(f"Training CSV missing columns: {missing}")

        x_data = df[FEATURE_NAMES]
        y_data = df["label"]

        x_train, x_test, y_train, y_test = train_test_split(
            x_data, y_data, test_size=0.2, random_state=42, stratify=y_data
        )

        clf = RandomForestClassifier(n_estimators=100, max_depth=8, random_state=42)
        clf.fit(x_train, y_train)

        report = classification_report(y_test, clf.predict(x_test))
        logger.info("Model evaluation:\n%s", report)

        model_path.parent.mkdir(parents=True, exist_ok=True)
        joblib.dump(clf, model_path)
        logger.info("Saved model to %s", model_path)
