import abc
import asyncio
import json
import logging
import os
import time
from typing import Dict, Optional

from .common import CsvLogger, TelemetryRecord, parse_telemetry_line
from .simulator import SimulatorCommandSender, SimulatorTelemetrySource

logger = logging.getLogger("swap.telemetry")

# NOTE: there is deliberately no ingest-time staleness drop here. Node A's
# ts_ms is ESP32 millis() -- time since that board's own boot, not epoch time
# (confirmed from swap_node's firmware: no RTC, no NTP sync) -- so it can
# never be validly compared against Python's recv_ts (real epoch seconds). An
# earlier revision did that comparison anyway and it silently discarded every
# frame ever received (age computed in the billions of seconds). The real
# stale-telemetry-rejection rule from the decision contract is enforced
# correctly downstream, in link_quality_model.FeatureWindow.eligible_records,
# using each record's recv_ts (set below from Python's own clock) -- that is
# the single source of truth for frame freshness.


class CommandSender(abc.ABC):
    @abc.abstractmethod
    async def force_protocol(self, node: str, protocol: int) -> None:
        pass


class TelemetrySource(abc.ABC):
    @abc.abstractmethod
    async def run(self, out_queue: "asyncio.Queue[TelemetryRecord]") -> None:
        pass

    @abc.abstractmethod
    def command_sender(self) -> CommandSender:
        pass

    def counters(self) -> Dict[str, object]:
        """Per-frame ingest counters for /status. Default: not applicable."""
        return {}


class BridgeCommandSender(CommandSender):
    """Sends force_protocol commands to the sketch over the Router Bridge,
    which writes them down Serial3 to Node A. Uses Bridge.call (not notify)
    so a failed/timed-out write surfaces as an exception instead of silently
    vanishing — Bridge.call is a blocking call, so it runs in the default
    executor to avoid blocking the asyncio event loop."""

    async def force_protocol(self, node: str, protocol: int) -> None:
        # Local import: arduino.app_utils only exists inside App Lab's own
        # Python environment on the board. Deferring the import here (rather
        # than at module level) means this module still imports cleanly off
        # the board — e.g. running the simulator locally — since this
        # function is only ever called when SWAP_INPUT_MODE=serial.
        from arduino.app_utils import Bridge

        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, lambda: Bridge.call("force_protocol", node, int(protocol), timeout=5))


class BridgeTelemetrySource(TelemetrySource):
    """Receives Node A's (and, piggybacked, Node B's) telemetry via the
    Router Bridge: the sketch reads Serial1 (D0/D1) and calls
    Bridge.notify("telemetry_line", line) per JSON line; here we register
    the matching Bridge.provide handler. No raw device is opened from
    Python — the sketch owns the UART entirely."""

    def __init__(self, csv_logger: CsvLogger):
        self._csv_logger = csv_logger
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._queue: Optional["asyncio.Queue[TelemetryRecord]"] = None
        self.frames_ok = 0
        self.frames_malformed = 0
        self.frames_dropped_stale = 0
        # Bridge's connection to the router is a persistent Unix socket, and
        # the UART to Node A is a fixed physical link, not a hot-pluggable
        # USB device — no reconnect event is exposed at this API level to
        # count, so this stays 0 rather than fabricating a number.
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
            "frames_dropped_stale": self.frames_dropped_stale,
            "reconnect_count": self.reconnect_count,
            "last_frame_age_s": last_frame_age_s,
        }

    def _on_telemetry_line(self, line: str) -> None:
        """Runs on Bridge's own background read thread, not the asyncio
        event loop — hands records off via call_soon_threadsafe."""
        recv_ts = time.time()
        try:
            raw = json.loads(line)
        except (json.JSONDecodeError, TypeError):
            self.frames_malformed += 1
            logger.warning("Dropping malformed JSON line: %r", str(line)[:200])
            return

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

    async def run(self, out_queue: "asyncio.Queue[TelemetryRecord]") -> None:
        # Local import — see BridgeCommandSender.force_protocol for why.
        from arduino.app_utils import Bridge

        self._loop = asyncio.get_running_loop()
        self._queue = out_queue
        Bridge.provide("telemetry_line", self._on_telemetry_line)
        logger.info("Bridge telemetry source ready, listening for 'telemetry_line' notifications")
        # No Bridge.unprovide() on the way out: this router build doesn't
        # implement "$/unregister" ("method $/unregister not available"),
        # so calling it here raised and crashed FastAPI's shutdown handler.
        # The process is exiting anyway, so leaving the handler registered
        # for the remaining lifetime is harmless.
        await asyncio.Event().wait()


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
