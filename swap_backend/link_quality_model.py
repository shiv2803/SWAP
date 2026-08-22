import logging
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Deque, Dict, List, Optional

import joblib
import numpy as np
import sklearn

from .common import PROTOCOL_BLE, PROTOCOL_LORA, PROTOCOL_NAMES, PROTOCOL_WIFI, TelemetryRecord

logger = logging.getLogger("swap.model")

# Canonical raw-column order. FEATURE_NAMES (the *_mean columns the model is
# trained and queried on) is derived from this single list so training
# (train_model.py) and inference (FeatureWindow below) can never drift apart
# on column order — that drift is the single most likely silent failure mode
# in this system.
RAW_FEATURE_COLUMNS = [
    "wifi_rssi",
    "wifi_loss",
    "ble_rssi",
    "lora_rssi",
    "lora_snr",
    "lora_loss",
    "rtt_ms",
]
FEATURE_NAMES = [f"{col}_mean" for col in RAW_FEATURE_COLUMNS]

MODEL_PATH = Path("models") / "link_quality_model.joblib"

# Minimum predict_proba() confidence required to accept a model prediction.
# Below this, we don't trust the forest and fall through to rule_based.
CONFIDENCE_GATE = 0.60

# A node's window must have a full WINDOW_SIZE (see LinkQualityModel.__init__)
# of samples AND its newest sample must be younger than this before the model
# is even asked to predict. Otherwise -> rule_based.
STALE_SECONDS = 30.0

# Minimum time between two *accepted* protocol switches for the same node,
# enforced by the SwitchGovernor (see LinkQualityModel._apply_switch_governor).
# This is also the switch-rate limit: a 5000 ms floor already caps a node to
# at most 12 accepted switches/minute, so a separate rate counter would be
# redundant. Bypassed entirely when the currently-active protocol has failed
# outright (see _is_total_loss) — a dead link must not wait out the dwell
# window before we're allowed to move off it.
DWELL_MS = 5000.0

# Per-protocol thresholds for "this link has failed, not just degraded."
# There's no explicit link-down flag in the telemetry schema, so this is a
# judgment call on the existing *_loss / *_rssi fields: WiFi/LoRa expose a
# loss fraction, BLE does not, so BLE uses an RSSI floor as its proxy instead.
WIFI_TOTAL_LOSS_THRESHOLD = 0.95
LORA_TOTAL_LOSS_THRESHOLD = 0.95
BLE_TOTAL_LOSS_RSSI = -95.0

# Score-margin (arbitrary score units) the *candidate* protocol must beat the
# *currently active* protocol by before we recommend a handover. Without this,
# noisy RSSI near a threshold causes the recommendation to flap every frame
# (WiFi -> BLE -> WiFi ...), which is disruptive in a live demo and wastes
# radio time on real hardware. Mirrors HYSTERESIS_MARGIN in the frontend's
# linkQualityPredictor.js so live and demo mode *feel* identical.
HYSTERESIS_MARGIN = 3.5

# Softmax temperature for converting raw scores into a confidence-like
# probability distribution across the three protocols. Higher = flatter
# distribution. Matches the frontend's client-side predictor for visual
# consistency between Demo and Live mode.
SOFTMAX_TEMPERATURE = 10.0

# Score normalization range used to compute the 0-100% Link Quality Index
# (LQI) shown in the UI health bar. Empirically chosen band; not a
# calibrated RF metric — purely a UX signal, same range used client-side.
LQI_SCORE_MIN = -150.0
LQI_SCORE_MAX = -30.0


@dataclass
class Decision:
    protocol: int
    confidence: float
    source: str  # "model" or "rule_based" — underlying inference path
    raw_scores: Dict[str, float] = field(default_factory=dict)
    probabilities: Dict[int, float] = field(default_factory=dict)
    lqi: int = 0
    hysteresis_guarded: bool = False
    dwell_guarded: bool = False
    total_loss_override: bool = False
    best_candidate: int = 0


class FeatureWindow:
    def __init__(self, window_size: int):
        self._window: Deque[TelemetryRecord] = deque(maxlen=window_size)

    def push(self, record: TelemetryRecord) -> None:
        self._window.append(record)

    def is_ready(self, min_samples: int) -> bool:
        return len(self._window) >= min_samples

    def is_full(self) -> bool:
        return len(self._window) == self._window.maxlen

    def latest_recv_ts(self) -> float:
        if not self._window:
            return 0.0
        return self._window[-1].recv_ts

    def to_feature_vector(self) -> np.ndarray:
        return self._means_array().reshape(1, -1)

    def _means_array(self) -> np.ndarray:
        arr = np.array(
            [[getattr(r, col) for col in RAW_FEATURE_COLUMNS] for r in self._window]
        )
        return arr.mean(axis=0)

    def means_dict(self) -> Dict[str, float]:
        """Named feature means, used for score computation and duty-cycle-free
        rule evaluation. Kept separate from to_feature_vector() (which the
        sklearn model consumes positionally) to avoid ambiguity/ordering bugs."""
        m = self._means_array()
        return dict(zip(RAW_FEATURE_COLUMNS, (float(v) for v in m)))

    def latest_active_protocol(self) -> int:
        """Active protocol as last self-reported by the node firmware itself
        (not our own prior recommendation) — this mirrors the frontend's
        hysteresis reference point (record.active_protocol) exactly, so both
        sides make the same handover-vs-hold decision given the same data."""
        if not self._window:
            return PROTOCOL_WIFI
        return self._window[-1].active_protocol


def _score_wifi(m: Dict[str, float]) -> float:
    # Kept numerically identical to train_model.py's _score_wifi and to
    # linkQualityPredictor.js's scoreWifi() — single source of truth for the
    # scoring formula is documented in SWAP_Project_Documentation §9.
    return m["wifi_rssi"] - (m["wifi_loss"] * 120.0) - (m["rtt_ms"] * 0.1)


def _score_ble(m: Dict[str, float]) -> float:
    return m["ble_rssi"] - (m["wifi_loss"] * 90.0) - (m["rtt_ms"] * 0.15)


def _score_lora(m: Dict[str, float]) -> float:
    return m["lora_rssi"] + (m["lora_snr"] * 2.0) - (m["lora_loss"] * 160.0) - (m["rtt_ms"] * 0.02)


def _compute_raw_scores(m: Dict[str, float]) -> Dict[int, float]:
    return {
        PROTOCOL_WIFI: _score_wifi(m),
        PROTOCOL_BLE: _score_ble(m),
        PROTOCOL_LORA: _score_lora(m),
    }


def _softmax_probabilities(scores: Dict[int, float]) -> Dict[int, float]:
    values = np.array(list(scores.values()))
    shifted = values - values.max()
    exp = np.exp(shifted / SOFTMAX_TEMPERATURE)
    probs = exp / exp.sum()
    return {proto: float(p) for proto, p in zip(scores.keys(), probs)}


def _lqi_from_score(score: float) -> int:
    normalized = ((score - LQI_SCORE_MIN) / (LQI_SCORE_MAX - LQI_SCORE_MIN)) * 100.0
    return int(max(0, min(100, round(normalized))))


def _is_total_loss(protocol: int, means: Dict[str, float]) -> bool:
    if protocol == PROTOCOL_WIFI:
        return means["wifi_loss"] >= WIFI_TOTAL_LOSS_THRESHOLD
    if protocol == PROTOCOL_LORA:
        return means["lora_loss"] >= LORA_TOTAL_LOSS_THRESHOLD
    if protocol == PROTOCOL_BLE:
        return means["ble_rssi"] <= BLE_TOTAL_LOSS_RSSI
    return False


def _major_minor(version: str) -> str:
    parts = version.split(".")
    return ".".join(parts[:2]) if len(parts) >= 2 else version


class RuleBasedFallback:
    WIFI_RSSI_MIN = -75.0
    WIFI_LOSS_MAX = 0.10
    BLE_RSSI_MIN = -85.0

    def decide(self, features: np.ndarray) -> int:
        """Returns the raw candidate protocol only — hysteresis and scoring
        are applied uniformly downstream in LinkQualityModel, regardless of
        whether the candidate came from the rule fallback or the trained model."""
        wifi_rssi, wifi_loss, ble_rssi, _, _, _, _ = features.flatten()
        if wifi_rssi > self.WIFI_RSSI_MIN and wifi_loss < self.WIFI_LOSS_MAX:
            return PROTOCOL_WIFI
        if ble_rssi > self.BLE_RSSI_MIN:
            return PROTOCOL_BLE
        return PROTOCOL_LORA


class LinkQualityModel:
    def __init__(
        self,
        model_path: Path = MODEL_PATH,
        window_size: int = 10,
        min_samples: int = 3,
    ):
        self._fallback = RuleBasedFallback()
        self._model_path = model_path
        self._bundle = self._try_load(model_path)
        self._model = self._bundle["model"] if self._bundle else None
        self._window_size = window_size
        self._min_samples = min_samples
        self._windows: Dict[str, FeatureWindow] = {}
        self._inference_times_ms: Deque[float] = deque(maxlen=100)
        # node_id -> (governed_protocol, accepted_switch_ts) for the dwell timer.
        self._governor: Dict[str, tuple] = {}

    @staticmethod
    def _try_load(model_path: Path) -> Optional[dict]:
        try:
            bundle = joblib.load(model_path)
        except FileNotFoundError:
            logger.info("No trained model found at %s, using rule_based fallback", model_path)
            return None
        except Exception as exc:
            logger.error(
                "Failed to load model bundle at %s (%s) — degrading to rule_based", model_path, exc
            )
            return None

        if not isinstance(bundle, dict) or "model" not in bundle:
            logger.error(
                "Model file at %s is not a valid bundle (expected dict with 'model' key) — "
                "degrading to rule_based",
                model_path,
            )
            return None

        bundle_features = bundle.get("feature_names")
        if bundle_features != FEATURE_NAMES:
            logger.error(
                "Model feature_names mismatch: bundle=%s runtime=%s — degrading to rule_based",
                bundle_features,
                FEATURE_NAMES,
            )
            return None

        bundle_sklearn = str(bundle.get("sklearn_version", ""))
        runtime_sklearn = sklearn.__version__
        if _major_minor(bundle_sklearn) != _major_minor(runtime_sklearn):
            logger.error(
                "scikit-learn version mismatch: bundle=%s runtime=%s — degrading to rule_based",
                bundle_sklearn,
                runtime_sklearn,
            )
            return None

        logger.info(
            "Loaded model bundle from %s (provenance=%s, trained_at=%s, sklearn=%s, n_train_rows=%s)",
            model_path,
            bundle.get("provenance"),
            bundle.get("trained_at"),
            bundle_sklearn,
            bundle.get("n_train_rows"),
        )
        return bundle

    def model_info(self) -> Dict[str, object]:
        if self._bundle is None:
            return {"loaded": False, "model_path": str(self._model_path)}

        meta = {k: v for k, v in self._bundle.items() if k != "model"}
        estimator = self._bundle["model"]
        importances: Dict[str, float] = {}
        if hasattr(estimator, "feature_importances_"):
            importances = {
                name: round(float(imp), 4)
                for name, imp in zip(FEATURE_NAMES, estimator.feature_importances_)
            }
        mean_inference_ms = (
            round(sum(self._inference_times_ms) / len(self._inference_times_ms), 3)
            if self._inference_times_ms
            else None
        )
        return {
            "loaded": True,
            "model_path": str(self._model_path),
            **meta,
            "feature_importances": importances,
            "mean_inference_ms": mean_inference_ms,
            "inference_samples": len(self._inference_times_ms),
            "confidence_gate": CONFIDENCE_GATE,
            "stale_seconds": STALE_SECONDS,
        }

    def _window_for(self, node_id: str) -> FeatureWindow:
        if node_id not in self._windows:
            self._windows[node_id] = FeatureWindow(window_size=self._window_size)
        return self._windows[node_id]

    def observe(self, record: TelemetryRecord) -> Optional[Decision]:
        window = self._window_for(record.node)
        window.push(record)
        if not window.is_ready(self._min_samples):
            return None
        return self._predict_from_window(record.node, window)

    def recommend_for_node(self, node_id: str) -> Optional[Decision]:
        window = self._windows.get(node_id)
        if window is None or not window.is_ready(self._min_samples):
            return None
        return self._predict_from_window(node_id, window)

    def _apply_switch_governor(
        self,
        node_id: str,
        decision: Decision,
        means: Dict[str, float],
        raw_scores: Dict[int, float],
    ) -> Decision:
        now = time.time()
        state = self._governor.get(node_id)

        if state is None:
            # First decision ever for this node — nothing to dwell against.
            self._governor[node_id] = (decision.protocol, now)
            return decision

        governed_protocol, last_switch_ts = state

        if decision.protocol == governed_protocol:
            return decision  # holding steady, no switch proposed

        total_loss = _is_total_loss(governed_protocol, means)
        elapsed_ms = (now - last_switch_ts) * 1000.0

        if total_loss or elapsed_ms >= DWELL_MS:
            self._governor[node_id] = (decision.protocol, now)
            decision.total_loss_override = total_loss
            return decision

        # Still inside the dwell window and the current link hasn't failed
        # outright — hold at the governed protocol instead of switching.
        decision.protocol = governed_protocol
        decision.lqi = _lqi_from_score(raw_scores[governed_protocol])
        decision.dwell_guarded = True
        return decision

    def _predict_from_window(self, node_id: str, window: FeatureWindow) -> Decision:
        means = window.means_dict()
        raw_scores = _compute_raw_scores(means)
        probabilities = _softmax_probabilities(raw_scores)
        best_candidate = max(raw_scores, key=raw_scores.get)

        model_eligible = (
            self._model is not None
            and window.is_full()
            and (time.time() - window.latest_recv_ts()) < STALE_SECONDS
        )

        candidate: int
        confidence: float
        source: str

        if model_eligible:
            try:
                features = window.to_feature_vector()
                start = time.perf_counter()
                pred = int(self._model.predict(features)[0])
                proba = self._model.predict_proba(features)[0]
                elapsed_ms = (time.perf_counter() - start) * 1000.0
                self._inference_times_ms.append(elapsed_ms)
                model_confidence = float(np.max(proba))
            except Exception as exc:
                logger.error(
                    "Model inference failed on features=%s (%s) — degrading to rule_based",
                    means,
                    exc,
                )
                candidate = self._fallback.decide(window.to_feature_vector())
                confidence = probabilities[candidate]
                source = "rule_based"
            else:
                if model_confidence >= CONFIDENCE_GATE:
                    candidate = pred
                    confidence = model_confidence
                    source = "model"
                else:
                    candidate = self._fallback.decide(window.to_feature_vector())
                    confidence = probabilities[candidate]
                    source = "rule_based"
        else:
            candidate = self._fallback.decide(window.to_feature_vector())
            confidence = probabilities[candidate]
            source = "rule_based"

        # --- Hysteresis guard ---------------------------------------------
        # Only override the currently-active protocol if the candidate beats
        # it by more than HYSTERESIS_MARGIN. Prevents rapid flapping on noisy
        # RSSI near a threshold. If guarded, we recommend *holding* the
        # active protocol instead of the raw candidate.
        active_protocol = window.latest_active_protocol()
        final_protocol = candidate
        hysteresis_guarded = False
        if candidate != active_protocol:
            active_score = raw_scores.get(active_protocol, -999.0)
            candidate_score = raw_scores.get(candidate, -999.0)
            if (candidate_score - active_score) < HYSTERESIS_MARGIN:
                final_protocol = active_protocol
                hysteresis_guarded = True

        lqi = _lqi_from_score(raw_scores[final_protocol])

        decision = Decision(
            protocol=final_protocol,
            confidence=confidence,
            source=source,
            raw_scores={
                "wifi": round(raw_scores[PROTOCOL_WIFI], 2),
                "ble": round(raw_scores[PROTOCOL_BLE], 2),
                "lora": round(raw_scores[PROTOCOL_LORA], 2),
            },
            probabilities={k: round(v, 3) for k, v in probabilities.items()},
            lqi=lqi,
            hysteresis_guarded=hysteresis_guarded,
            best_candidate=best_candidate,
        )
        return self._apply_switch_governor(node_id, decision, means, raw_scores)

    @staticmethod
    def train_and_save(
        csv_path: str,
        model_path: Path = MODEL_PATH,
        window: int = 10,
        provenance: str = "bootstrap-heuristic",
    ) -> Dict[str, object]:
        import subprocess

        import pandas as pd
        from sklearn.ensemble import RandomForestClassifier
        from sklearn.metrics import classification_report, confusion_matrix
        from sklearn.model_selection import train_test_split

        df = pd.read_csv(csv_path)
        required_columns: List[str] = FEATURE_NAMES + ["label"]
        missing = [column for column in required_columns if column not in df.columns]
        if missing:
            raise ValueError(f"Training CSV missing columns: {missing}")

        x_data = df[FEATURE_NAMES]
        y_data = df["label"]

        for label, count in y_data.value_counts().items():
            if count < 20:
                logger.warning(
                    "Class %s has only %d training windows (<20) — model may be unreliable for it",
                    label,
                    count,
                )

        x_train, x_test, y_train, y_test = train_test_split(
            x_data, y_data, test_size=0.2, random_state=42, stratify=y_data
        )

        clf = RandomForestClassifier(
            n_estimators=100, max_depth=8, random_state=42, class_weight="balanced"
        )
        # Fit on bare ndarrays, not the DataFrame, so the estimator does not
        # retain feature_names_in_. Inference always calls predict() with a
        # plain ndarray (FeatureWindow.to_feature_vector()) for speed — if the
        # estimator remembers pandas column names, sklearn emits a
        # UserWarning on every single prediction call, which would spam the
        # log continuously in production. Column order is still guaranteed
        # by RAW_FEATURE_COLUMNS being the single source for both sides.
        clf.fit(x_train.to_numpy(), y_train)

        y_pred = clf.predict(x_test.to_numpy())
        report = classification_report(y_test, y_pred)
        matrix = confusion_matrix(y_test, y_pred)
        importances = dict(zip(FEATURE_NAMES, clf.feature_importances_.tolist()))

        logger.info("Classification report:\n%s", report)
        logger.info("Confusion matrix (rows=true, cols=pred):\n%s", matrix)
        logger.info("Feature importances: %s", importances)

        try:
            git_rev = (
                subprocess.check_output(
                    ["git", "rev-parse", "--short", "HEAD"], stderr=subprocess.DEVNULL
                )
                .decode()
                .strip()
            )
        except Exception:
            git_rev = "unversioned"

        bundle = {
            "model": clf,
            "feature_names": FEATURE_NAMES,
            "classes": [PROTOCOL_NAMES[c].lower() for c in clf.classes_.tolist()],
            "sklearn_version": sklearn.__version__,
            "window": window,
            "provenance": provenance,
            "n_train_rows": int(len(df)),
            "trained_at": datetime.now(timezone.utc).isoformat(),
            "git_rev": git_rev,
        }

        if provenance == "bootstrap-heuristic":
            logger.warning(
                "provenance=bootstrap-heuristic: this model was trained on simulator "
                "output labelled by the hand-tuned scoring heuristics above. It has "
                "learned to approximate those heuristics, not real RF physics. Its "
                "honest value is proving the inference pipeline end to end (load, "
                "feature extraction, predict_proba, confidence gate, hysteresis, "
                "dashboard) — it is a bootstrap artifact to be retrained on measured "
                "data. Do not present its accuracy score as evidence of link-prediction "
                "skill."
            )

        model_path.parent.mkdir(parents=True, exist_ok=True)
        joblib.dump(bundle, model_path)
        logger.info("Saved model bundle to %s", model_path)

        return {
            "report": report,
            "confusion_matrix": matrix.tolist(),
            "feature_importances": importances,
        }
