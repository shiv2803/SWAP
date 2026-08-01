import csv
import logging
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, Optional

logger = logging.getLogger("swap.common")

PROTOCOL_WIFI = 0
PROTOCOL_BLE = 1
PROTOCOL_LORA = 2
PROTOCOL_NAMES = {
    PROTOCOL_WIFI: "WIFI",
    PROTOCOL_BLE: "BLE",
    PROTOCOL_LORA: "LORA",
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
            return TelemetryRecord(
                recv_ts=recv_ts,
                node=str(raw["node"]).lower(),
                ts_ms=int(raw["ts_ms"]),
                active_protocol=int(raw["active_protocol"]),
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

