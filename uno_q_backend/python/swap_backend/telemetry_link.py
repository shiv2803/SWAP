import abc
import asyncio
import json
import logging
import os
import time
from typing import Dict, Optional, Union

from .common import CsvLogger, TelemetryRecord, parse_telemetry_line
from .simulator import SimulatorCommandSender, SimulatorTelemetrySource

logger = logging.getLogger("swap.telemetry")


class CommandSender(abc.ABC):
    @abc.abstractmethod
    async def force_protocol(self, node: str, protocol: int) -> None:
        pass


class TelemetrySource(abc.ABC):
    @abc.abstractmethod
    async def run(self, out_queue: "asyncio.Queue[Union[TelemetryRecord, dict]]") -> None:
        pass

    @abc.abstractmethod
    def command_sender(self) -> CommandSender:
        pass

    def counters(self) -> Dict[str, object]:
        return {}


ARDUINO_IMPORT_HINT = (
    "SWAP_INPUT_MODE=serial needs the 'arduino' module, which only exists inside "
    "App Lab's Python environment on the UNO Q. Run this backend on the board "
    "(uno_q_backend), or set SWAP_INPUT_MODE=sim to run it off-board."
)


def _import_bridge():
    """Import the App Lab Bridge, or fail with a message that says what to do.

    A bare ModuleNotFoundError here reads as a broken install rather than
    "you are on the wrong machine", which is exactly how it looked when this
    was run on Windows.
    """
    try:
        from arduino.app_utils import Bridge
    except ModuleNotFoundError as exc:
        raise RuntimeError(ARDUINO_IMPORT_HINT) from exc
    return Bridge


def _recover_json_object(line: str) -> Optional[dict]:
    """Best-effort recovery of one complete JSON object from a corrupted line.

    UART corruption (a competing console on the same pins, or a baud blip)
    shows up as truncated fragments glued to the front of a good frame:
    {"li{"li{"link_state":...}. The tail is usually intact, so scan forward
    from each opening brace and keep the first fragment that decodes whole.
    Prefers dropping the garbage prefix over dropping the whole telemetry frame.
    """
    if not isinstance(line, str):
        return None
    decoder = json.JSONDecoder()
    start = line.find("{")
    while start != -1:
        try:
            obj, _ = decoder.raw_decode(line[start:])
        except ValueError:
            start = line.find("{", start + 1)
            continue
        return obj if isinstance(obj, dict) else None
    return None


class BridgeCommandSender(CommandSender):
    async def force_protocol(self, node: str, protocol: int) -> None:
        Bridge = _import_bridge()
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, lambda: Bridge.call("force_protocol", node, int(protocol), timeout=5))


class BridgeTelemetrySource(TelemetrySource):
    def __init__(self, csv_logger: CsvLogger):
        self._csv_logger = csv_logger
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._queue: Optional["asyncio.Queue[Union[TelemetryRecord, dict]]"] = None
        self.frames_ok = 0
        self.frames_malformed = 0
        self.frames_recovered = 0
        self._last_malformed_log = 0.0
        self.frames_dropped_stale = 0
        self.reconnect_count = 0
        self._last_frame_recv_ts: Optional[float] = None

    def command_sender(self) -> CommandSender:
        return BridgeCommandSender()

    def counters(self) -> Dict[str, object]:
        last_frame_age_s = (
            round(time.time() - self._last_frame_recv_ts, 3) if self._last_frame_recv_ts is not None else None
        )
        return {
            "frames_ok": self.frames_ok,
            "frames_malformed": self.frames_malformed,
            "frames_recovered": self.frames_recovered,
            "frames_dropped_stale": self.frames_dropped_stale,
            "reconnect_count": self.reconnect_count,
            "last_frame_age_s": last_frame_age_s,
        }

    def _on_telemetry_line(self, line: str) -> None:
        recv_ts = time.time()
        try:
            raw = json.loads(line)
        except (json.JSONDecodeError, TypeError):
            raw = _recover_json_object(line)
            if raw is None:
                self.frames_malformed += 1
                # Corruption arrives at the frame rate, so an unthrottled
                # warning per line buries every other log message.
                if recv_ts - self._last_malformed_log >= 5.0:
                    self._last_malformed_log = recv_ts
                    logger.warning("Dropping malformed JSON line: %r", str(line)[:200])
                return
            self.frames_recovered += 1

        # ---- HANDLE INITIALIZATION EVENTS ----
        if raw.get("event") == "node_initialized":
            node = str(raw.get("node", "")).lower()
            if node in ("a", "b"):
                self._publish({"type": "node_initialized", "node": node, "recv_ts": recv_ts})
            return

        # ---- HANDLE SWITCH RESULTS ----
        # Node A's verdict on a commanded switch (success / failed / rejected).
        # These used to fall through to parse_telemetry_line and be counted as
        # malformed, so the backend never learned whether a trial worked and
        # every transition ended by timing out -- i.e. every successful switch
        # was recorded as a failure.
        if raw.get("cmd") == "switch_result":
            self._publish({
                "type": "switch_result",
                "status": str(raw.get("status", "")).lower(),
                # "attempted" on success/failure, "requested" on a rejection.
                "attempted": raw.get("attempted", raw.get("requested")),
                "active": raw.get("active"),
                "reason": raw.get("reason", ""),
                "recv_ts": recv_ts,
            })
            return

        # ---- NORMAL TELEMETRY ----
        records = parse_telemetry_line(raw, recv_ts=recv_ts)
        if not records:
            self.frames_malformed += 1
            return

        for record in records:
            self.frames_ok += 1
            self._last_frame_recv_ts = recv_ts
            self._csv_logger.append(record)
            if self._loop is not None and self._queue is not None:
                self._loop.call_soon_threadsafe(self._queue.put_nowait, record)

    def _publish(self, event: dict) -> None:
        """Hand a control event to the asyncio loop from the Bridge's thread."""
        if self._loop is None or self._queue is None:
            return
        self._loop.call_soon_threadsafe(self._queue.put_nowait, event)

    async def run(self, out_queue: "asyncio.Queue[Union[TelemetryRecord, dict]]") -> None:
        Bridge = _import_bridge()
        self._loop = asyncio.get_running_loop()
        self._queue = out_queue
        Bridge.provide("telemetry_line", self._on_telemetry_line)
        logger.info("Bridge telemetry source ready, listening for 'telemetry_line' notifications")
        await asyncio.Event().wait()


class SimulatorCommandSender(CommandSender):
    def __init__(self, sim_source: "SimulatorTelemetrySource"):
        self._sim_source = sim_source

    async def force_protocol(self, node: str, protocol: int) -> None:
        self._sim_source.force_protocol(node, protocol)


def create_source_from_env() -> TelemetrySource:
    csv_log_path = os.environ.get("SWAP_CSV_LOG", "telemetry_log.csv")
    csv_logger = CsvLogger(csv_log_path)

    input_mode = os.environ.get("SWAP_INPUT_MODE", "sim").strip().lower()
    if input_mode == "serial":
        logger.info("Using Bridge telemetry input (Node A UART relayed by sketch.ino over Serial1/D0-D1)")
        return BridgeTelemetrySource(csv_logger=csv_logger)

    logger.info("Using simulator telemetry input")
    sim_source = SimulatorTelemetrySource(csv_logger=csv_logger)
    sender = SimulatorCommandSender(sim_source)
    sim_source.attach_sender(sender)
    return sim_source