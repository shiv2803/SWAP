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
]


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
          - wifi_loss / lora_loss: this schema has no packet-loss measurement
            at all. Left at the "unknown" sentinel (1.0) rather than guessed.
          - rtt_ms: this schema has no round-trip measurement. link_airtime_ms
            is the closest available proxy and is used as a stand-in, but it
            is airtime, not RTT — the RuleBasedFallback/scoring formulas'
            rtt_ms weighting was tuned assuming true RTT.
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
            rtt_ms = float(raw.get("link_airtime_ms", 0.0))
        except (KeyError, TypeError, ValueError) as exc:
            logger.warning("Dropping malformed pinout-format telemetry frame: %s (%s)", raw, exc)
            return []

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
            elif active_protocol == PROTOCOL_BLE:
                fields["ble_rssi"] = rssi
            else:
                fields["lora_rssi"] = rssi
                fields["lora_snr"] = snr
            return fields

        records = [
            TelemetryRecord(
                recv_ts=recv_ts,
                node="a",
                ts_ms=ts_ms,
                active_protocol=active_protocol,
                rtt_ms=rtt_ms,
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
                        ts_ms=ts_ms,
                        active_protocol=active_protocol,
                        rtt_ms=rtt_ms,
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
