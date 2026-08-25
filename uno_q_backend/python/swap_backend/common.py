import csv
import logging
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

logger = logging.getLogger("swap.common")

PROTOCOL_WIFI = 0
PROTOCOL_BLE = 1
PROTOCOL_LORA = 2
PROTOCOL_NAMES = {
    PROTOCOL_WIFI: "WIFI",
    PROTOCOL_BLE: "BLE",
    PROTOCOL_LORA: "LORA",
}

# Wire format sends active_protocol as a lowercase string ("wifi"|"ble"|"lora"),
# not the internal int id. This is the single place that mapping happens.
PROTOCOL_NAME_TO_ID = {
    "wifi": PROTOCOL_WIFI,
    "ble": PROTOCOL_BLE,
    "lora": PROTOCOL_LORA,
}

CSV_FIELDS = [
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
    "link_connected",
    # --- Extended perception (multi_radio_perception_data spec) ---
    # Reported by the firmware but NOT model features. The trained model's
    # feature vector is still exactly the 7 fields above (FEATURE_NAMES in
    # link_quality_model.py) -- these are carried for display/diagnostics and
    # for future retraining, deliberately without changing model inputs.
    "crc_ok",
    "time_on_air_ms",
    "tx_power_dbm",
    "channel",
    "spreading_factor",
    "bandwidth_khz",
    "coding_rate",
    # Measured by the firmware's probe/ACK loop (spec section 28).
    "packet_loss",
    "packet_success_rate",
    "latency_ms",
    "max_rtt_ms",
    "jitter_ms",
    "throughput_bps",
    "stability_db",
    "probes_sent",
    "probes_acked",
]


def _optional_bool(value: Any) -> Optional[bool]:
    """Wire booleans that may arrive as true/false, "true"/"false", or 1/0.

    Returns None when the field is absent, which the control layer treats as
    "this firmware does not report connectivity" rather than "disconnected".
    """
    if value is None:
        return None
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    text = str(value).strip().lower()
    if text in ("true", "1", "yes", "connected"):
        return True
    if text in ("false", "0", "no", "disconnected"):
        return False
    return None


@dataclass
class TelemetryRecord:
    recv_ts: float
    node: str
    ts_ms: int
    active_protocol: int
    wifi_rssi: float
    wifi_loss: float
    ble_rssi: float
    lora_rssi: float
    lora_snr: float
    lora_loss: float
    rtt_ms: int

    # Node A's own verdict on whether the A<->B link is up (the "connected"
    # field of the v2 wire format). The control state machine gates every
    # switch command on this -- inferring connectivity from "a protocol is
    # active" is not the same thing and is always true in practice.
    link_connected: Optional[bool] = None

    # Extended perception fields -- all optional with "unknown" defaults so
    # older frames (and the from_json wire format, which doesn't carry them)
    # still parse unchanged. None means "this radio/frame doesn't report it",
    # never a fabricated value.
    crc_ok: Optional[bool] = None
    time_on_air_ms: Optional[float] = None
    tx_power_dbm: Optional[float] = None
    channel: Optional[int] = None
    spreading_factor: Optional[int] = None
    bandwidth_khz: Optional[float] = None
    coding_rate: Optional[int] = None

    # Probe/ACK loop measurements. Absent until at least one probe has
    # resolved, so an unmeasured window reads as None rather than a
    # misleading 0% loss. Note wifi_loss/lora_loss above stay at their 1.0
    # "unknown" sentinel -- packet_loss here is the real, measured figure.
    packet_loss: Optional[float] = None
    packet_success_rate: Optional[float] = None
    latency_ms: Optional[float] = None
    max_rtt_ms: Optional[float] = None
    jitter_ms: Optional[float] = None
    throughput_bps: Optional[float] = None
    stability_db: Optional[float] = None
    probes_sent: Optional[int] = None
    probes_acked: Optional[int] = None

    @staticmethod
    def from_json(raw: Dict[str, Any], recv_ts: float) -> Optional["TelemetryRecord"]:
        try:
            active_protocol_raw = raw["active_protocol"]
            if isinstance(active_protocol_raw, str):
                active_protocol = PROTOCOL_NAME_TO_ID[active_protocol_raw.strip().lower()]
            else:
                active_protocol = int(active_protocol_raw)
            return TelemetryRecord(
                recv_ts=recv_ts,
                node=str(raw["node"]).lower(),
                ts_ms=int(raw["ts_ms"]),
                active_protocol=active_protocol,
                wifi_rssi=float(raw.get("wifi_rssi", -999.0)),
                wifi_loss=float(raw.get("wifi_loss", 1.0)),
                ble_rssi=float(raw.get("ble_rssi", -999.0)),
                lora_rssi=float(raw.get("lora_rssi", -999.0)),
                lora_snr=float(raw.get("lora_snr", -999.0)),
                lora_loss=float(raw.get("lora_loss", 1.0)),
                rtt_ms=int(raw.get("rtt_ms", 0)),
                link_connected=_optional_bool(raw.get("connected")),
            )
        except (KeyError, TypeError, ValueError) as exc:
            logger.warning("Dropping malformed telemetry frame: %s (%s)", raw, exc)
            return None

    @staticmethod
    def from_pinout_json(raw: Dict[str, Any], recv_ts: float) -> List["TelemetryRecord"]:
        """Parses the v2 hardware doc's wire format: a single active link_state/
        link_rssi (Node A only measures whatever protocol is currently active,
        not all three at once) plus an optional nested node_b_telemetry object.
        Returns 0-2 records (Node A always if parseable, Node B only if present).

        Known, deliberate approximations (not fabricated data — explicitly
        marked with the same "missing" sentinels from_json already uses for
        absent fields):
          - wifi_loss / lora_loss: the firmware's probe/ACK loop now measures
            real packet loss for the ACTIVE protocol, and that value is used
            here. The two inactive protocols still have no loss data and keep
            the "unknown" sentinel (1.0). Frames without a packet_loss field
            (older firmware) keep the sentinel for all three.
          - rtt_ms: now a real round-trip measurement when the frame carries
            one. Older frames without it fall back to time_on_air_ms, which
            is airtime rather than RTT — a proxy, not the same quantity.
          - Whichever of wifi_rssi/ble_rssi/lora_rssi is NOT the currently
            active link on this record is left at the "unknown" sentinel
            (-999.0), since Node A's real firmware doesn't measure it. This
            means the 10-record rolling mean for a protocol that hasn't been
            active recently will be dominated by -999.0 sentinels, not a real
            reading — a structural gap between what this hardware reports and
            what the original 3-simultaneous-RSSI feature vector assumes.
        """
        try:
            active_protocol = PROTOCOL_NAME_TO_ID[str(raw["link_state"]).strip().lower()]
            ts_ms = int(float(raw["timestamp"]) * 1000)
            link_rssi = float(raw["link_rssi"])
            link_snr = float(raw.get("link_snr", -999.0))
            # The firmware emits "time_on_air_ms"; an earlier revision of this
            # parser only looked for "link_airtime_ms", which silently left
            # rtt_ms at 0.0 for every real frame. Both accepted now, new name
            # first.
            # Prefer the probe loop's real round-trip measurement; fall back
            # to the airtime proxy only for older frames that predate it.
            if raw.get("rtt_ms") is not None:
                rtt_ms = float(raw["rtt_ms"])
            else:
                rtt_ms = float(raw.get("time_on_air_ms", raw.get("link_airtime_ms", 0.0)))
        except (KeyError, TypeError, ValueError) as exc:
            logger.warning("Dropping malformed pinout-format telemetry frame: %s (%s)", raw, exc)
            return []

        def _opt(source: Dict[str, Any], key: str, caster):
            """Reads an optional perception field, returning None (not a
            fabricated default) when it's absent or unparseable."""
            if key not in source or source[key] is None:
                return None
            try:
                return caster(source[key])
            except (TypeError, ValueError):
                return None

        # Radio config is a property of the shared link, so it applies to both
        # nodes' records for this frame.
        shared_extras = {
            "tx_power_dbm": _opt(raw, "tx_power_dbm", float),
            "spreading_factor": _opt(raw, "spreading_factor", int),
            "bandwidth_khz": _opt(raw, "bandwidth_khz", float),
            "coding_rate": _opt(raw, "coding_rate", int),
        }

        # Real packet loss from the firmware's probe/ACK loop, when present.
        # This is a genuine measurement of the ACTIVE link, so it replaces the
        # 1.0 "unknown" sentinel for that protocol only -- the inactive ones
        # still have no loss data and keep the sentinel.
        measured_loss = _opt(raw, "packet_loss", float)

        def _protocol_fields(rssi: float, snr: float) -> Dict[str, float]:
            fields = {
                "wifi_rssi": -999.0,
                "wifi_loss": 1.0,
                "ble_rssi": -999.0,
                "lora_rssi": -999.0,
                "lora_snr": -999.0,
                "lora_loss": 1.0,
            }
            if active_protocol == PROTOCOL_WIFI:
                fields["wifi_rssi"] = rssi
                if measured_loss is not None:
                    fields["wifi_loss"] = measured_loss
            elif active_protocol == PROTOCOL_BLE:
                fields["ble_rssi"] = rssi
                # The feature vector has no ble_loss column, so BLE's measured
                # loss rides in the record's packet_loss field only.
            else:
                fields["lora_rssi"] = rssi
                fields["lora_snr"] = snr
                if measured_loss is not None:
                    fields["lora_loss"] = measured_loss
            return fields

        # Probe/ACK measurements describe the shared A<->B link, so they apply
        # to both nodes' records for this frame.
        probe_stats = {
            "packet_loss": measured_loss,
            "packet_success_rate": _opt(raw, "packet_success_rate", float),
            "latency_ms": _opt(raw, "latency_ms", float),
            "max_rtt_ms": _opt(raw, "max_rtt_ms", float),
            "jitter_ms": _opt(raw, "jitter_ms", float),
            "throughput_bps": _opt(raw, "throughput_bps", float),
            "stability_db": _opt(raw, "stability_db", float),
            "probes_sent": _opt(raw, "probes_sent", int),
            "probes_acked": _opt(raw, "probes_acked", int),
        }

        link_connected = _optional_bool(raw.get("connected"))

        records = [
            TelemetryRecord(
                recv_ts=recv_ts,
                node="a",
                link_connected=link_connected,
                ts_ms=ts_ms,
                active_protocol=active_protocol,
                rtt_ms=rtt_ms,
                crc_ok=_opt(raw, "crc_ok", bool),
                time_on_air_ms=_opt(raw, "time_on_air_ms", float),
                channel=_opt(raw, "channel", int),
                **probe_stats,
                **shared_extras,
                **_protocol_fields(link_rssi, link_snr),
            )
        ]

        node_b_raw = raw.get("node_b_telemetry")
        if node_b_raw:
            try:
                # Node A/B share a single peer-to-peer adaptive link, not two
                # independent connections, so B's reading is on the same
                # active_protocol as A's for this message.
                b_rssi = float(node_b_raw["rssi"])
                b_snr = float(node_b_raw.get("snr", -999.0))
                records.append(
                    TelemetryRecord(
                        recv_ts=recv_ts,
                        node="b",
                        link_connected=link_connected,
                        ts_ms=ts_ms,
                        active_protocol=active_protocol,
                        rtt_ms=rtt_ms,
                        crc_ok=_opt(node_b_raw, "crc_ok", bool),
                        time_on_air_ms=_opt(node_b_raw, "time_on_air_ms", float),
                        channel=_opt(node_b_raw, "channel", int),
                        **probe_stats,
                        **shared_extras,
                        **_protocol_fields(b_rssi, b_snr),
                    )
                )
            except (KeyError, TypeError, ValueError) as exc:
                logger.warning("Dropping malformed node_b_telemetry: %s (%s)", node_b_raw, exc)

        return records


def parse_telemetry_line(raw: Dict[str, Any], recv_ts: float) -> List[TelemetryRecord]:
    """Single entry point for turning one parsed JSON line into 0-2
    TelemetryRecords, accepting both wire formats seen on this project:
    the original schema (ts_ms/active_protocol/wifi_rssi/...) and the v2
    hardware doc's schema (timestamp/link_state/link_rssi/node_b_telemetry).
    Detected by which distinguishing key is present.
    """
    if "link_state" in raw:
        return TelemetryRecord.from_pinout_json(raw, recv_ts=recv_ts)
    record = TelemetryRecord.from_json(raw, recv_ts=recv_ts)
    return [record] if record is not None else []


class CsvLogger:
    def __init__(self, path: str):
        self._path = Path(path)
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._write_header_if_needed()

    def _write_header_if_needed(self) -> None:
        if not self._path.exists() or self._path.stat().st_size == 0:
            with self._path.open("w", newline="", encoding="utf-8") as file_obj:
                writer = csv.DictWriter(file_obj, fieldnames=CSV_FIELDS)
                writer.writeheader()

    def append(self, record: TelemetryRecord) -> None:
        with self._path.open("a", newline="", encoding="utf-8") as file_obj:
            writer = csv.DictWriter(file_obj, fieldnames=CSV_FIELDS)
            writer.writerow(asdict(record))
