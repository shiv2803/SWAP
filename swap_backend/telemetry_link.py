import abc
import asyncio
import json
import logging
import os
import time
from typing import Dict

from .common import CsvLogger, TelemetryRecord
from .simulator import SimulatorCommandSender, SimulatorTelemetrySource

logger = logging.getLogger("swap.telemetry")

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


class SerialCommandSender(CommandSender):
    def __init__(self, writers: Dict[str, asyncio.StreamWriter]):
        self._writers = writers

    async def force_protocol(self, node: str, protocol: int) -> None:
        writer = self._writers.get(node)
        if writer is None:
            raise RuntimeError("Node is not connected on serial")
        payload = json.dumps({"cmd": "force_protocol", "protocol": int(protocol)})
        writer.write((payload + "\n").encode("utf-8"))
        await writer.drain()


class SerialTelemetrySource(TelemetrySource):
    def __init__(
        self,
        node_ports: Dict[str, str],
        baud_rate: int,
        csv_logger: CsvLogger,
    ):
        self._node_ports = node_ports
        self._baud_rate = baud_rate
        self._csv_logger = csv_logger
        self._writers: Dict[str, asyncio.StreamWriter] = {}

    def command_sender(self) -> CommandSender:
        return SerialCommandSender(self._writers)

    async def run(self, out_queue: "asyncio.Queue[TelemetryRecord]") -> None:
        await asyncio.gather(
            self._node_loop("a", self._node_ports["a"], out_queue),
            self._node_loop("b", self._node_ports["b"], out_queue),
        )

    async def _node_loop(
        self,
        node: str,
        port: str,
        out_queue: "asyncio.Queue[TelemetryRecord]",
        retry_delay_s: float = 2.0,
    ) -> None:
        try:
            import serial_asyncio
        except ImportError as exc:
            raise RuntimeError(
                "pyserial-asyncio is required for SWAP_INPUT_MODE=serial"
            ) from exc

        while True:
            try:
                reader, writer = await serial_asyncio.open_serial_connection(
                    url=port,
                    baudrate=self._baud_rate,
                )
                self._writers[node] = writer
                logger.info("Connected node=%s on %s @ %d", node, port, self._baud_rate)
                await self._read_loop(node, reader, out_queue)
            except (FileNotFoundError, OSError) as exc:
                logger.warning("Serial open failed node=%s port=%s (%s)", node, port, exc)
            except Exception as exc:
                logger.error("Serial loop failed node=%s: %s", node, exc)
            await asyncio.sleep(retry_delay_s)

    async def _read_loop(
        self,
        node: str,
        reader: asyncio.StreamReader,
        out_queue: "asyncio.Queue[TelemetryRecord]",
    ) -> None:
        while True:
            line = await reader.readline()
            if not line:
                raise ConnectionError("EOF on serial stream")
            try:
                raw = json.loads(line.decode("utf-8", errors="replace").strip())
            except json.JSONDecodeError:
                continue
            raw.setdefault("node", node)
            raw.setdefault("ts_ms", int(time.time() * 1000))
            record = TelemetryRecord.from_json(raw, recv_ts=time.time())
            if record is None:
                continue
            self._csv_logger.append(record)
            await out_queue.put(record)


def create_source_from_env() -> TelemetrySource:
    csv_log_path = os.environ.get("SWAP_CSV_LOG", "telemetry_log.csv")
    csv_logger = CsvLogger(csv_log_path)

    input_mode = os.environ.get("SWAP_INPUT_MODE", "sim").strip().lower()
    if input_mode == "serial":
        node_ports = {
            "a": os.environ.get("SWAP_NODE_A_PORT", "/dev/ttyUSB0"),
            "b": os.environ.get("SWAP_NODE_B_PORT", "/dev/ttyUSB1"),
        }
        baud_rate = int(os.environ.get("SWAP_TELEMETRY_BAUD", "19200"))
        logger.info("Using serial telemetry input")
        return SerialTelemetrySource(node_ports=node_ports, baud_rate=baud_rate, csv_logger=csv_logger)

    logger.info("Using simulator telemetry input")
    sim_source = SimulatorTelemetrySource(csv_logger=csv_logger)
    sender = SimulatorCommandSender(sim_source)
    sim_source.attach_sender(sender)
    return sim_source