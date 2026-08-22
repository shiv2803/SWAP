import asyncio
import math
import random
import time
from dataclasses import dataclass
from typing import Dict, Optional

from .common import (
    PROTOCOL_BLE,
    PROTOCOL_LORA,
    PROTOCOL_WIFI,
    CsvLogger,
    TelemetryRecord,
)


@dataclass
class _NodePhase:
    wifi_rssi: float
    wifi_loss: float
    ble_rssi: float
    lora_rssi: float
    lora_snr: float
    lora_loss: float


class SimulatorCommandSender:
    def __init__(self, source: "SimulatorTelemetrySource"):
        self._source = source

    async def force_protocol(self, node: str, protocol: int) -> None:
        self._source.force_protocol(node=node, protocol=protocol)


class SimulatorTelemetrySource:
    def __init__(self, csv_logger: CsvLogger, interval_s: float = 0.25):
        self._csv_logger = csv_logger
        self._interval_s = interval_s
        self._forced_protocol: Dict[str, int] = {}
        self._sender: Optional[SimulatorCommandSender] = None

    def attach_sender(self, sender: SimulatorCommandSender) -> None:
        self._sender = sender

    def command_sender(self) -> SimulatorCommandSender:
        if self._sender is None:
            raise RuntimeError("Simulator command sender is not attached")
        return self._sender

    def force_protocol(self, node: str, protocol: int) -> None:
        if protocol not in (PROTOCOL_WIFI, PROTOCOL_BLE, PROTOCOL_LORA):
            raise ValueError("Invalid protocol")
        self._forced_protocol[node] = protocol

    async def run(self, out_queue: "asyncio.Queue[TelemetryRecord]") -> None:
        await asyncio.gather(
            self._node_loop("a", phase_offset=0.0, out_queue=out_queue),
            self._node_loop("b", phase_offset=3.2, out_queue=out_queue),
        )

    async def _node_loop(
        self,
        node: str,
        phase_offset: float,
        out_queue: "asyncio.Queue[TelemetryRecord]",
    ) -> None:
        while True:
            now = time.time()
            phase = self._phase(t=now + phase_offset)
            active_protocol = self._forced_protocol.get(node, self._choose_local_protocol(phase))
            rtt_ms = self._estimate_rtt_ms(phase=phase, active_protocol=active_protocol)
            record = TelemetryRecord(
                recv_ts=now,
                node=node,
                ts_ms=int(now * 1000),
                active_protocol=active_protocol,
                wifi_rssi=self._noise(phase.wifi_rssi, 2.0),
                wifi_loss=self._bounded(self._noise(phase.wifi_loss, 0.02), 0.0, 1.0),
                ble_rssi=self._noise(phase.ble_rssi, 2.0),
                lora_rssi=self._noise(phase.lora_rssi, 1.5),
                lora_snr=self._noise(phase.lora_snr, 1.0),
                lora_loss=self._bounded(self._noise(phase.lora_loss, 0.01), 0.0, 1.0),
                rtt_ms=rtt_ms,
            )
            self._csv_logger.append(record)
            await out_queue.put(record)
            await asyncio.sleep(self._interval_s)

    def _phase(self, t: float) -> _NodePhase:
        cycle = (math.sin(t / 18.0) + 1.0) / 2.0
        wifi_rssi = -60.0 - 25.0 * cycle
        wifi_loss = 0.02 + 0.38 * cycle
        lora_rssi = -90.0 - 7.0 * cycle
        lora_snr = 7.0 - 4.0 * cycle
        lora_loss = 0.01 + 0.07 * cycle

        # BLE rides its own independent cycle (different period + phase
        # offset) instead of the same `cycle` as WiFi/LoRa. On a shared
        # cycle, BLE's RSSI always sits on the same slope between WiFi's and
        # LoRa's, so it can never be the best-scoring link: WiFi wins
        # whenever conditions are good, LoRa wins whenever they're bad, and
        # BLE is never optimal at either extreme — confirmed empirically,
        # zero BLE-labelled windows across 43k real training windows.
        # Decoupling it lets BLE be independently "good" while WiFi is
        # degraded, giving it genuine windows where it's the right call.
        ble_cycle = (math.sin(t / 11.0 + 2.4) + 1.0) / 2.0
        ble_rssi = -70.0 - 18.0 * ble_cycle
        return _NodePhase(
            wifi_rssi=wifi_rssi,
            wifi_loss=wifi_loss,
            ble_rssi=ble_rssi,
            lora_rssi=lora_rssi,
            lora_snr=lora_snr,
            lora_loss=lora_loss,
        )

    @staticmethod
    def _choose_local_protocol(phase: _NodePhase) -> int:
        if phase.wifi_rssi > -75.0 and phase.wifi_loss < 0.10:
            return PROTOCOL_WIFI
        if phase.ble_rssi > -85.0:
            return PROTOCOL_BLE
        return PROTOCOL_LORA

    @staticmethod
    def _estimate_rtt_ms(phase: _NodePhase, active_protocol: int) -> int:
        if active_protocol == PROTOCOL_WIFI:
            base = 7.0 + (phase.wifi_loss * 20.0)
        elif active_protocol == PROTOCOL_BLE:
            base = 25.0 + (phase.wifi_loss * 60.0)
        else:
            base = 220.0 + (phase.lora_loss * 400.0)
        return int(max(1.0, base + random.uniform(-3.0, 3.0)))

    @staticmethod
    def _noise(value: float, sigma: float) -> float:
        return float(value + random.gauss(0.0, sigma))

    @staticmethod
    def _bounded(value: float, low: float, high: float) -> float:
        return max(low, min(high, value))