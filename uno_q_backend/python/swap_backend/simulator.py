"""Off-hardware telemetry source that emulates the node contract.

This is not just a data generator: it plays the ESP32 side of
SWAP_UNO_Q_Control_Logic_Specification.md so the UNO Q control logic can be
exercised without the boards — the boot handshake (§0), a single shared A<->B
link with a CONNECTED/DISCONNECTED state (§2), the 5-second commanded trial
with success/rollback and a switch_result report (§5-§7), and autonomous
link-loss recovery.

Node A and Node B are two ends of ONE adaptive link, so they always report the
same active protocol and the same connectivity — an earlier version let each
node pick its own protocol independently, which cannot happen on the hardware.
"""

import asyncio
import logging
import math
import random
import time
from dataclasses import dataclass
from typing import Dict, Optional, Union

from .common import (
    PROTOCOL_BLE,
    PROTOCOL_LORA,
    PROTOCOL_NAMES,
    PROTOCOL_WIFI,
    CsvLogger,
    TelemetryRecord,
)

logger = logging.getLogger("swap.simulator")


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
    # Mirrors the firmware's own timings so the control logic sees the same
    # shape of events it will see on hardware.
    TRIAL_S = 5.0
    RECOVERY_S = 5.0
    # How long into the trial the nodes take to reach a verdict. Well inside
    # TRIAL_S: a real switch that works announces itself as soon as the new
    # link carries traffic, it does not wait out the window.
    TRIAL_VERDICT_S = 1.2
    BOOT_A_S = 0.5      # Node A announces itself shortly after the backend starts
    BOOT_B_S = 1.5      # Node B's BOOT:B:LORA reaches Node A a little later
    LINK_UP_S = 2.5     # first successful LoRa exchange -> link CONNECTED

    def __init__(self, csv_logger: CsvLogger, interval_s: float = 0.25):
        self._csv_logger = csv_logger
        self._interval_s = interval_s
        self._sender: Optional[SimulatorCommandSender] = None
        self._queue: Optional["asyncio.Queue"] = None

        # Shared link state (the nodes' side of the world, not the UNO Q's).
        self._started_at = time.time()
        self._active_protocol = PROTOCOL_LORA   # boot protocol (§1)
        self._previous_protocol = PROTOCOL_LORA
        self._link_connected = False
        self._trial_target: Optional[int] = None
        self._trial_started_at = 0.0
        self._recovery_started_at: Optional[float] = None

    def attach_sender(self, sender: SimulatorCommandSender) -> None:
        self._sender = sender

    def command_sender(self) -> SimulatorCommandSender:
        if self._sender is None:
            raise RuntimeError("Simulator command sender is not attached")
        return self._sender

    def force_protocol(self, node: str, protocol: int) -> None:
        """Stands in for Node A receiving a force_protocol command."""
        if protocol not in (PROTOCOL_WIFI, PROTOCOL_BLE, PROTOCOL_LORA):
            raise ValueError("Invalid protocol")
        if self._trial_target is not None:
            # The firmware refuses a second switch mid-trial and says so.
            self._emit_control({
                "type": "switch_result",
                "status": "rejected",
                "attempted": protocol,
                "active": self._active_protocol,
                "reason": "switch_in_progress",
            })
            return
        self._trial_target = protocol
        self._trial_started_at = time.time()
        logger.info("[sim] trial started: %s -> %s", PROTOCOL_NAMES[self._active_protocol], PROTOCOL_NAMES[protocol])

    def counters(self) -> Dict[str, object]:
        return {
            "mode": "simulator",
            "sim_active_protocol": PROTOCOL_NAMES[self._active_protocol],
            "sim_link_connected": self._link_connected,
        }

    # ------------------------------------------------------------------

    async def run(self, out_queue: "asyncio.Queue[Union[TelemetryRecord, dict]]") -> None:
        self._queue = out_queue
        self._started_at = time.time()

        announced_a = False
        announced_b = False

        while True:
            now = time.time()
            uptime = now - self._started_at

            # --- §0 boot handshake ------------------------------------
            if not announced_a and uptime >= self.BOOT_A_S:
                announced_a = True
                self._emit_control({"type": "node_initialized", "node": "a"})
            if not announced_b and uptime >= self.BOOT_B_S:
                announced_b = True
                self._emit_control({"type": "node_initialized", "node": "b"})

            # --- §2 the boot link comes up ----------------------------
            if not self._link_connected and self._recovery_started_at is None and uptime >= self.LINK_UP_S:
                self._link_connected = True
                logger.info("[sim] LoRa link established")

            phase = self._phase(now)
            self._service_trial(now, phase)
            self._service_recovery(now, phase)

            if announced_a:
                for record in self._build_records(now, phase):
                    self._csv_logger.append(record)
                    await out_queue.put(record)

            await asyncio.sleep(self._interval_s)

    # ------------------------------------------------------------------

    def _service_trial(self, now: float, phase: _NodePhase) -> None:
        """§5-§7: resolve a commanded switch inside its 5-second window."""
        if self._trial_target is None:
            return
        elapsed = now - self._trial_started_at
        if elapsed < self.TRIAL_VERDICT_S:
            return

        target = self._trial_target
        succeeded = self._protocol_usable(target, phase)
        if not succeeded and elapsed < self.TRIAL_S:
            return  # keep trying until the window actually expires

        self._trial_target = None
        if succeeded:
            self._previous_protocol = self._active_protocol
            self._active_protocol = target
            self._link_connected = True
            logger.info("[sim] trial SUCCESS on %s", PROTOCOL_NAMES[target])
            self._emit_control({
                "type": "switch_result",
                "status": "success",
                "attempted": target,
                "active": target,
            })
        else:
            # The nodes roll themselves back before reporting (§7).
            logger.info(
                "[sim] trial FAILED on %s, rolled back to %s",
                PROTOCOL_NAMES[target], PROTOCOL_NAMES[self._active_protocol],
            )
            self._emit_control({
                "type": "switch_result",
                "status": "failed",
                "attempted": target,
                "active": self._active_protocol,
            })

    def _service_recovery(self, now: float, phase: _NodePhase) -> None:
        """Link-loss recovery: 5 seconds on the same protocol, then rollback."""
        if self._trial_target is not None:
            return

        usable = self._protocol_usable(self._active_protocol, phase)

        if self._recovery_started_at is None:
            if self._link_connected and not usable:
                self._recovery_started_at = now
                self._link_connected = False
                logger.info("[sim] %s link lost -- recovery window open", PROTOCOL_NAMES[self._active_protocol])
            return

        if usable:
            self._recovery_started_at = None
            self._link_connected = True
            logger.info("[sim] %s link recovered", PROTOCOL_NAMES[self._active_protocol])
            return

        if now - self._recovery_started_at >= self.RECOVERY_S:
            failed = self._active_protocol
            self._active_protocol = self._previous_protocol
            self._recovery_started_at = None
            self._link_connected = True
            logger.info(
                "[sim] %s did not recover -- rolled back to %s",
                PROTOCOL_NAMES[failed], PROTOCOL_NAMES[self._active_protocol],
            )

    def _emit_control(self, event: Dict[str, object]) -> None:
        if self._queue is None:
            return
        event = dict(event)
        event.setdefault("recv_ts", time.time())
        self._queue.put_nowait(event)

    # ------------------------------------------------------------------

    def _build_records(self, now: float, phase: _NodePhase):
        """One frame per node, both describing the same shared link."""
        records = []
        for node, offset in (("a", 0.0), ("b", 1.7)):
            node_phase = self._phase(now + offset)
            records.append(
                TelemetryRecord(
                    recv_ts=now,
                    node=node,
                    ts_ms=int(now * 1000),
                    active_protocol=self._active_protocol,
                    link_connected=self._link_connected,
                    wifi_rssi=self._noise(node_phase.wifi_rssi, 2.0),
                    wifi_loss=self._bounded(self._noise(node_phase.wifi_loss, 0.02), 0.0, 1.0),
                    ble_rssi=self._noise(node_phase.ble_rssi, 2.0),
                    lora_rssi=self._noise(node_phase.lora_rssi, 1.5),
                    lora_snr=self._noise(node_phase.lora_snr, 1.0),
                    lora_loss=self._bounded(self._noise(node_phase.lora_loss, 0.01), 0.0, 1.0),
                    rtt_ms=self._estimate_rtt_ms(node_phase, self._active_protocol),
                )
            )
        return records

    def _phase(self, t: float) -> _NodePhase:
        cycle = (math.sin(t / 18.0) + 1.0) / 2.0
        return _NodePhase(
            wifi_rssi=-60.0 - 25.0 * cycle,
            wifi_loss=0.02 + 0.38 * cycle,
            ble_rssi=-70.0 - 18.0 * cycle,
            lora_rssi=-90.0 - 7.0 * cycle,
            lora_snr=7.0 - 4.0 * cycle,
            lora_loss=0.01 + 0.07 * cycle,
        )

    @staticmethod
    def _protocol_usable(protocol: int, phase: _NodePhase) -> bool:
        """Whether a trial on this protocol would actually connect.

        LoRa is the floor: it is the boot protocol precisely because it comes
        up without a peer already listening on an SSID or GATT service, so it
        is always usable here and rollback always has somewhere to land.
        """
        if protocol == PROTOCOL_WIFI:
            return phase.wifi_rssi > -82.0 and phase.wifi_loss < 0.32
        if protocol == PROTOCOL_BLE:
            return phase.ble_rssi > -85.0
        return True

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
