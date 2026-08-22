import logging
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Deque, Dict, List, Optional, Tuple

import joblib
import numpy as np
import sklearn

from .common import PROTOCOL_BLE, PROTOCOL_LORA, PROTOCOL_NAME_TO_ID, PROTOCOL_WIFI, TelemetryRecord

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

MODEL_PATH = Path("models") / "link_quality_rf.joblib"

# Keys train_model.py's bundle dict must have; see 3B in the project spec.
REQUIRED_BUNDLE_KEYS = {
    "model",
    "feature_names",
    "classes",
    "sklearn_version",
    "window",
    "provenance",
    "n_train_rows",
    "trained_at",
    "git_rev",
}

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

# --- Switch governor constants (from the project's decision contract) ------
MIN_DWELL_S = 5.0  # minimum dwell between accepted protocol switches
CONFIDENCE_GATE = 0.60  # below this, a model decision falls through to rule_based
STALE_THRESHOLD_S = 30.0  # records older than this are excluded from inference
# A record whose wifi/ble/lora RSSI are all at TelemetryRecord.from_json's own
# "field absent" sentinel means the node has no live signal on any path at
# all — a total link loss, not just a bad-but-recoverable one path.
TOTAL_LINK_LOSS_RSSI_SENTINEL = -999.0


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
    best_candidate: int = 0


class FeatureWindow:
    def __init__(self, window_size: int):
        self._window: Deque[TelemetryRecord] = deque(maxlen=window_size)

    def push(self, record: TelemetryRecord) -> None:
        self._window.append(record)

    def eligible_records(self, now: float, max_age_s: float) -> List[TelemetryRecord]:
        """Records from the rolling window that are still fresh enough to be
        used for inference — the stale-telemetry-rejection rule."""
        return [r for r in self._window if (now - r.recv_ts) <= max_age_s]

    def latest_active_protocol(self) -> int:
        """Active protocol as last self-reported by the node firmware itself
        (not our own prior recommendation) — this mirrors the frontend's
        hysteresis reference point (record.active_protocol) exactly, so both
        sides make the same handover-vs-hold decision given the same data."""
        if not self._window:
            return PROTOCOL_WIFI
        return self._window[-1].active_protocol


def feature_means_from_records(records: List[TelemetryRecord]) -> Dict[str, float]:
    """The single source of truth for the 7-feature mean vector. train_model.py
    imports this directly instead of reimplementing it — feature-order drift
    between training and inference is the most likely silent failure here."""
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
            for r in records
        ]
    )
    m = arr.mean(axis=0)
    return {
        "wifi_rssi": float(m[0]),
        "wifi_loss": float(m[1]),
        "ble_rssi": float(m[2]),
        "lora_rssi": float(m[3]),
        "lora_snr": float(m[4]),
        "lora_loss": float(m[5]),
        "rtt_ms": float(m[6]),
    }


def feature_vector_from_means(m: Dict[str, float]) -> np.ndarray:
    # Order MUST match FEATURE_NAMES / train_model.py exactly.
    return np.array(
        [[m["wifi_rssi"], m["wifi_loss"], m["ble_rssi"], m["lora_rssi"], m["lora_snr"], m["lora_loss"], m["rtt_ms"]]]
    )


def _score_wifi(m: Dict[str, float]) -> float:
    # Kept numerically identical to linkQualityPredictor.js's scoreWifi() —
    # single source of truth for the scoring formula is documented in
    # SWAP_Project_Documentation §9. train_model.py imports compute_raw_scores
    # rather than reimplementing these, for the same anti-drift reason as
    # feature_means_from_records above.
    return m["wifi_rssi"] - (m["wifi_loss"] * 120.0) - (m["rtt_ms"] * 0.1)


def _score_ble(m: Dict[str, float]) -> float:
    # Was previously penalized by wifi_loss (a copy/paste artifact from
    # _score_wifi) instead of any BLE-specific loss term -- the wire schema
    # has no ble_loss field to use instead. That meant BLE's score cratered
    # whenever WiFi degraded, i.e. exactly the situation where a handover to
    # BLE would be wanted, and it never won a single training window across
    # the full telemetry_log.csv (see audit, 2026-08-22). Removing the
    # misapplied penalty rather than inventing a substitute metric.
    return m["ble_rssi"] - (m["rtt_ms"] * 0.15)


def _score_lora(m: Dict[str, float]) -> float:
    return m["lora_rssi"] + (m["lora_snr"] * 2.0) - (m["lora_loss"] * 160.0) - (m["rtt_ms"] * 0.02)


def compute_raw_scores(m: Dict[str, float]) -> Dict[int, float]:
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


def _is_total_link_loss(m: Dict[str, float]) -> bool:
    return (
        m["wifi_rssi"] <= TOTAL_LINK_LOSS_RSSI_SENTINEL
        and m["ble_rssi"] <= TOTAL_LINK_LOSS_RSSI_SENTINEL
        and m["lora_rssi"] <= TOTAL_LINK_LOSS_RSSI_SENTINEL
    )


def _tree_leaf_proba(tree, x: np.ndarray) -> np.ndarray:
    """Walks one DecisionTreeClassifier's internal arrays directly (feature,
    threshold, children_left/right, value) to the leaf for x, and returns
    that leaf's class-proportion vector. Same result as tree.predict_proba
    (a leaf is children_left == -1), just without sklearn's per-call
    validation/dispatch machinery."""
    t = tree.tree_
    node = 0
    while t.children_left[node] != -1:
        node = t.children_left[node] if x[t.feature[node]] <= t.threshold[node] else t.children_right[node]
    leaf_counts = t.value[node][0]
    return leaf_counts / leaf_counts.sum()


def _forest_predict_proba(forest, x: np.ndarray) -> np.ndarray:
    """RandomForestClassifier.predict_proba is the mean of every tree's leaf
    proportions, in forest.classes_ order. Replicates exactly that, one
    sample at a time, without going through joblib's Parallel dispatch."""
    total = np.zeros(len(forest.classes_))
    for estimator in forest.estimators_:
        total += _tree_leaf_proba(estimator, x)
    return total / len(forest.estimators_)


def _sklearn_major_minor(version: str) -> str:
    parts = str(version).split(".")
    return ".".join(parts[:2])


class RuleBasedFallback:
    WIFI_RSSI_MIN = -75.0
    WIFI_LOSS_MAX = 0.10
    BLE_RSSI_MIN = -85.0

    def decide(self, features: np.ndarray) -> int:
        """Returns the raw candidate protocol only — hysteresis and dwell
        are applied uniformly downstream in LinkQualityModel, regardless of
        whether the candidate came from the rule fallback or the trained model."""
        wifi_rssi, wifi_loss, ble_rssi, _, _, _, _ = features.flatten()
        if wifi_rssi > self.WIFI_RSSI_MIN and wifi_loss < self.WIFI_LOSS_MAX:
            return PROTOCOL_WIFI
        if ble_rssi > self.BLE_RSSI_MIN:
            return PROTOCOL_BLE
        return PROTOCOL_LORA


@dataclass
class _GovernorState:
    last_protocol: Optional[int] = None
    last_switch_ts: float = 0.0


class SwitchGovernor:
    """Rate-limits accepted protocol switches per node. A candidate protocol
    change is only accepted once at least MIN_DWELL_S has elapsed since the
    last accepted switch for that node — this single dwell check is also the
    switch-rate limiter (it caps the rate at 1 / MIN_DWELL_S). Total link
    loss is the one condition allowed to bypass the dwell timer, since
    holding a dead protocol for the sake of not-flapping defeats the point."""

    def __init__(self, min_dwell_s: float = MIN_DWELL_S):
        self._min_dwell_s = min_dwell_s
        self._state: Dict[str, _GovernorState] = {}

    def _state_for(self, node_id: str) -> _GovernorState:
        if node_id not in self._state:
            self._state[node_id] = _GovernorState()
        return self._state[node_id]

    def gate(self, node_id: str, candidate: int, now: float, total_link_loss: bool) -> Tuple[int, bool]:
        """Returns (accepted_protocol, dwell_guarded)."""
        state = self._state_for(node_id)

        if state.last_protocol is None:
            state.last_protocol = candidate
            state.last_switch_ts = now
            return candidate, False

        if candidate == state.last_protocol:
            return candidate, False

        if total_link_loss or (now - state.last_switch_ts) >= self._min_dwell_s:
            state.last_protocol = candidate
            state.last_switch_ts = now
            return candidate, False

        return state.last_protocol, True


class LinkQualityModel:
    def __init__(
        self,
        model_path: Path = MODEL_PATH,
        window_size: int = 10,
        governor: Optional[SwitchGovernor] = None,
    ):
        self._fallback = RuleBasedFallback()
        self._model_path = model_path
        self._bundle = self._try_load(model_path)
        self._model = self._bundle["model"] if self._bundle is not None else None
        self._window_size = window_size
        self._windows: Dict[str, FeatureWindow] = {}
        self._governor = governor if governor is not None else SwitchGovernor()
        self._inference_count = 0
        self._inference_total_s = 0.0

    @staticmethod
    def _try_load(model_path: Path) -> Optional[dict]:
        try:
            bundle = joblib.load(model_path)
        except FileNotFoundError:
            logger.info("No trained model found at %s, using rule-based fallback", model_path)
            return None
        except Exception as exc:
            logger.error("Failed to load model bundle at %s (%s) — falling back to rule_based", model_path, exc)
            return None

        if not isinstance(bundle, dict) or not REQUIRED_BUNDLE_KEYS.issubset(bundle.keys()):
            logger.error(
                "Model bundle at %s is missing required keys (expected %s) — falling back to rule_based",
                model_path,
                sorted(REQUIRED_BUNDLE_KEYS),
            )
            return None

        if list(bundle["feature_names"]) != FEATURE_NAMES:
            logger.error(
                "Model bundle feature_names mismatch: bundle=%s runtime=%s — falling back to rule_based",
                bundle["feature_names"],
                FEATURE_NAMES,
            )
            return None

        bundle_sk = _sklearn_major_minor(bundle["sklearn_version"])
        runtime_sk = _sklearn_major_minor(sklearn.__version__)
        if bundle_sk != runtime_sk:
            logger.error(
                "Model bundle sklearn_version mismatch: bundle=%s (%s) runtime=%s (%s) — falling back to rule_based",
                bundle["sklearn_version"],
                bundle_sk,
                sklearn.__version__,
                runtime_sk,
            )
            return None

        logger.info(
            "Loaded trained model from %s (provenance=%s, trained_at=%s, n_train_rows=%d, git_rev=%s, sklearn=%s)",
            model_path,
            bundle["provenance"],
            bundle["trained_at"],
            bundle["n_train_rows"],
            bundle["git_rev"],
            bundle["sklearn_version"],
        )
        return bundle

    def _window_for(self, node_id: str) -> FeatureWindow:
        if node_id not in self._windows:
            self._windows[node_id] = FeatureWindow(window_size=self._window_size)
        return self._windows[node_id]

    def observe(self, record: TelemetryRecord) -> Optional[Decision]:
        window = self._window_for(record.node)
        window.push(record)
        return self._predict_from_window(record.node, window)

    def recommend_for_node(self, node_id: str) -> Optional[Decision]:
        window = self._windows.get(node_id)
        if window is None:
            return None
        return self._predict_from_window(node_id, window)

    def model_info(self) -> Dict[str, object]:
        if self._bundle is None:
            return {"loaded": False, "model_path": str(self._model_path)}

        try:
            importances = {
                name: round(float(imp), 4)
                for name, imp in zip(self._bundle["feature_names"], self._bundle["model"].feature_importances_)
            }
        except Exception:
            importances = None

        mean_inference_ms = (
            round((self._inference_total_s / self._inference_count) * 1000.0, 4) if self._inference_count else None
        )

        return {
            "loaded": True,
            "model_path": str(self._model_path),
            "feature_names": self._bundle["feature_names"],
            "classes": self._bundle["classes"],
            "sklearn_version": self._bundle["sklearn_version"],
            "window": self._bundle["window"],
            "provenance": self._bundle["provenance"],
            "n_train_rows": self._bundle["n_train_rows"],
            "trained_at": self._bundle["trained_at"],
            "git_rev": self._bundle["git_rev"],
            "feature_importances": importances,
            "inference_count": self._inference_count,
            "mean_inference_ms": mean_inference_ms,
        }

    def _run_model_inference(self, means: Dict[str, float]) -> Tuple[int, float]:
        """Returns (predicted_protocol, confidence). Times the call and
        accumulates the running mean so /model/info can report it."""
        # Uses _forest_predict_proba (hand-rolled leaf walk) instead of
        # self._model.predict_proba(): sklearn's predict_proba dispatches
        # every one of the forest's trees through joblib's Parallel/lock-
        # accumulation path even at n_jobs=1, which measured ~44ms/call on
        # this board's CPU at n_estimators=100 (confirmed via isolated
        # timeit — not the "X does not have valid feature names" warning
        # or its suppression, both measured negligible). The leaf walk
        # below computes the identical math (mean of each tree's leaf class
        # proportions) without that dispatch overhead; validated against
        # self._model.predict_proba to be numerically identical before
        # switching this over (2026-08-22).
        features = feature_vector_from_means(means)
        t0 = time.perf_counter()
        proba = _forest_predict_proba(self._model, features[0])
        elapsed_s = time.perf_counter() - t0

        self._inference_count += 1
        self._inference_total_s += elapsed_s
        mean_ms = (self._inference_total_s / self._inference_count) * 1000.0
        logger.debug("Model inference took %.3f ms (running mean %.3f ms)", elapsed_s * 1000.0, mean_ms)

        classes = self._bundle["classes"]
        best_idx = int(np.argmax(proba))
        predicted_protocol = PROTOCOL_NAME_TO_ID[classes[best_idx]]
        confidence = float(proba[best_idx])
        return predicted_protocol, confidence

    def _predict_from_window(self, node_id: str, window: FeatureWindow) -> Optional[Decision]:
        now = time.time()

        # --- Stale telemetry rejection --------------------------------------
        # Records older than STALE_THRESHOLD_S don't count towards either the
        # feature means or the 10-record model-readiness check.
        eligible = window.eligible_records(now=now, max_age_s=STALE_THRESHOLD_S)
        if not eligible:
            return None

        means = feature_means_from_records(eligible)
        raw_scores = compute_raw_scores(means)
        probabilities = _softmax_probabilities(raw_scores)
        best_candidate = max(raw_scores, key=raw_scores.get)
        total_link_loss = _is_total_link_loss(means)

        # --- Model eligibility + confidence gate ----------------------------
        # A node needs a full, fresh window of `window_size` (10) records
        # before the trained model is used at all — below that, or when the
        # model's own confidence is below CONFIDENCE_GATE, the decision falls
        # through entirely to the deterministic rule-based path. Any inference
        # exception degrades to rule_based too — the system must never stop
        # making decisions because the model misbehaved.
        model_eligible = self._model is not None and len(eligible) >= self._window_size
        candidate: Optional[int] = None
        confidence: Optional[float] = None
        source = "rule_based"
        if model_eligible:
            try:
                pred_protocol, model_confidence = self._run_model_inference(means)
                if model_confidence >= CONFIDENCE_GATE:
                    candidate = pred_protocol
                    confidence = model_confidence
                    source = "model"
            except Exception as exc:
                logger.error("Model inference failed for node=%s, degrading to rule_based. features=%s error=%s",
                             node_id, means, exc)

        if candidate is None:
            candidate = self._fallback.decide(feature_vector_from_means(means))
            confidence = probabilities[candidate]
            source = "rule_based"

        # --- Hysteresis guard -------------------------------------------
        # Only override the currently-active protocol if the candidate beats
        # it by more than HYSTERESIS_MARGIN. Prevents rapid flapping on noisy
        # RSSI near a threshold. If guarded, the candidate we hand to the
        # switch governor is the currently active protocol instead.
        active_protocol = window.latest_active_protocol()
        pre_governor_protocol = candidate
        hysteresis_guarded = False
        if candidate != active_protocol:
            active_score = raw_scores.get(active_protocol, -999.0)
            candidate_score = raw_scores.get(candidate, -999.0)
            if (candidate_score - active_score) < HYSTERESIS_MARGIN:
                pre_governor_protocol = active_protocol
                hysteresis_guarded = True

        # --- Switch governor (time-based dwell across decisions) -----------
        final_protocol, dwell_guarded = self._governor.gate(
            node_id=node_id,
            candidate=pre_governor_protocol,
            now=now,
            total_link_loss=total_link_loss,
        )

        # `confidence` was computed for `candidate` (the model/rule-based pick
        # before hysteresis or the switch governor could override it). If
        # either guard changed the outcome, that confidence value no longer
        # describes the protocol we're actually recommending — re-derive it
        # from the same softmax distribution used for `probabilities` so the
        # two fields never disagree about which protocol they're describing.
        if final_protocol != candidate:
            confidence = probabilities[final_protocol]

        lqi = _lqi_from_score(raw_scores[final_protocol])

        return Decision(
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
            dwell_guarded=dwell_guarded,
            best_candidate=best_candidate,
        )
