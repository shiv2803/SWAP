"""SWAP UNO Q control logic — an explicit state machine.

Implements SWAP_UNO_Q_Control_Logic_Specification.md, which requires this be
"an explicit state machine rather than loosely coupled if/else conditions, so
that protocol state, transition state, connectivity state, and rollback state
cannot become inconsistent."

Division of labour (spec §12): this side decides *whether and when* to switch
and gates every command; the ESP32 nodes execute the switch, run the 5-second
trial, decide whether the new protocol actually works, roll back on failure,
and report the outcome. Node A is the only node with a UART to the UNO Q, so
everything about Node B — its boot handshake and its half of the link — is
relayed by Node A.

The sequence the spec mandates:

    UNO Q running -> Node A initialized -> Node B initialized (relayed)
    -> boot protocol LoRa -> link CONNECTED -> first command
    -> default target Wi-Fi -> 5s trial -> success/rollback
    -> normal adaptive operation
"""

import logging
import time
from dataclasses import dataclass
from enum import Enum
from typing import Dict, List, Optional, Tuple

from .common import PROTOCOL_BLE, PROTOCOL_LORA, PROTOCOL_NAMES, PROTOCOL_WIFI

logger = logging.getLogger("swap.control")


class SwapState(str, Enum):
    """States from the spec's high-level state machine (§10).

    BOOT           waiting for the node_initialized events. The UNO Q may
                   already have been running before the nodes booted, so it
                   must never assume initialization (§0).
    WAIT_LINK      both nodes initialized, boot protocol is LoRa, waiting for
                   Node A to report the A<->B link CONNECTED (§2). Being
                   initialized is not the same as being connected.
    READY_DEFAULT  link is up and no operating protocol has been commanded
                   yet. The first command must target Wi-Fi (§3).
    SWITCHING      a switch command is in flight and the nodes are inside
                   their 5-second trial. No further commands (§8).
    STABLE         normal adaptive operation (§11).
    RECOVERING     a previously working link dropped; the nodes are running
                   their own 5-second recovery window. No commands until it
                   resolves (link-loss recovery section).
    """

    BOOT = "boot"
    WAIT_LINK = "wait_link"
    READY_DEFAULT = "ready_default"
    SWITCHING = "switching"
    STABLE = "stable"
    RECOVERING = "recovering"


class Verdict(str, Enum):
    """What request_switch() decided to do with a switch request."""

    SEND = "send"          # gate open, caller should transmit the command now
    DEFERRED = "deferred"  # held in the pending slot, will be sent when the guard clears
    REJECTED = "rejected"  # gate closed for a reason that queuing cannot fix


@dataclass
class PendingCommand:
    """A command held back by the BLE settling guard.

    The spec is explicit that such a command "must not be discarded during
    this guard; it should remain pending and execute after the 3-second
    interval if the system is still stable and connected".
    """

    node: str
    protocol: int
    source: str
    queued_at: float


@dataclass
class GateResult:
    allowed: bool
    reason: str = ""
    # True when the only thing blocking the command is the BLE settling
    # guard, which elapses on its own — the caller should queue, not reject.
    deferrable: bool = False


@dataclass
class TransitionRecord:
    """One completed transition, kept for the event trail in /status."""

    at: float
    attempted: int
    outcome: str            # success | failed | rejected | timeout
    resulting: int
    source: str
    reason: str = ""


class SwapControlMachine:
    # --- Spec constants -------------------------------------------------
    # Every switch gets a 5-second connection-validation window (§5).
    TRANSITION_TIMEOUT_S = 5.0
    # The nodes spend up to ~1.5 s negotiating (Node A retries an "I:<N>"
    # intent until Node B acks) *before* their own 5 s trial starts, and the
    # switch_result then has to travel back over the UART. Declaring failure
    # at exactly 5.0 s would race a switch that is still legitimately in its
    # trial, so the local timeout is the spec window plus this grace. It is
    # only a backstop: the authoritative outcome is Node A's switch_result.
    RESULT_GRACE_S = 2.0
    # After a BLE connection is established, wait before allowing the next
    # switch command (BLE priority / post-connection guard section).
    BLE_SETTLE_S = 3.0
    # How long the adaptive layer stands down after an operator command. The
    # spec does not cover manual overrides at all -- but with §11 running
    # autonomously, an operator's choice was being reversed by the next
    # adaptive evaluation seconds later, which makes the manual control
    # useless. Long enough to see the chosen protocol actually work.
    OPERATOR_HOLD_S = 15.0
    # Boot/initialization protocol vs first operating protocol (§3, §13).
    BOOT_PROTOCOL = PROTOCOL_LORA
    DEFAULT_PROTOCOL = PROTOCOL_WIFI

    def __init__(self) -> None:
        # §0 initialization flags — independent, both required.
        self.node_a_initialized = False
        self.node_b_initialized = False

        # §10 state variables.
        self.state = SwapState.BOOT
        self.current_protocol = self.BOOT_PROTOCOL
        self.previous_protocol = self.BOOT_PROTOCOL
        self.requested_protocol: Optional[int] = None
        self.link_connected = False
        self.transition_start_time: float = 0.0
        self.transition_source: str = ""

        # Link-loss recovery.
        self.recovery_started_at: float = 0.0
        self.recovery_protocol: Optional[int] = None

        # BLE post-connection guard.
        self.ble_guard_until: float = 0.0
        # Adaptive stand-down after a manual override.
        self.operator_hold_until: float = 0.0

        self.pending: Optional[PendingCommand] = None
        self.default_protocol_commanded = False
        self.history: List[TransitionRecord] = []
        self.last_reason: str = "waiting for node initialization"

    # ------------------------------------------------------------------
    # Inputs from the telemetry stream
    # ------------------------------------------------------------------

    def on_node_initialized(self, node: str) -> None:
        """§0: a node announced its boot state.

        Node A reports itself over UART; Node B's event is relayed by Node A
        after the BOOT:B:LORA handshake, so both arrive on the same link.
        """
        node = node.strip().lower()
        if node == "a":
            if self.node_a_initialized:
                return
            self.node_a_initialized = True
        elif node == "b":
            if self.node_b_initialized:
                return
            self.node_b_initialized = True
        else:
            logger.warning("Ignoring node_initialized for unknown node %r", node)
            return

        logger.info(
            "Node %s initialized (A=%s B=%s)",
            node.upper(), self.node_a_initialized, self.node_b_initialized,
        )
        if self.state is SwapState.BOOT and self.both_nodes_initialized:
            self._enter(SwapState.WAIT_LINK, "both nodes initialized, waiting for the LoRa link")

    @property
    def both_nodes_initialized(self) -> bool:
        return self.node_a_initialized and self.node_b_initialized

    def on_link_report(self, connected: bool, reported_protocol: Optional[int], now: Optional[float] = None) -> None:
        """§2: Node A's report of A<->B connectivity, once per telemetry frame.

        `reported_protocol` is what the firmware says is actually running. The
        firmware is the authority on that (it executes switches and rolls back
        on its own), so outside a commanded transition the machine follows it
        rather than assuming its own bookkeeping is right.
        """
        now = time.time() if now is None else now
        was_connected = self.link_connected
        self.link_connected = connected

        if connected and not was_connected:
            logger.info("Link CONNECTED (protocol=%s)", _name(reported_protocol))
        elif was_connected and not connected:
            logger.info("Link DISCONNECTED (protocol=%s)", _name(reported_protocol))

        # Track an autonomous protocol change (a firmware-side rollback, or the
        # tail of a trial whose switch_result has not arrived yet). Never while
        # SWITCHING: there the reported protocol is mid-trial and unproven, and
        # overwriting previous_protocol with it would destroy the rollback target.
        if (
            reported_protocol is not None
            and reported_protocol >= 0
            and reported_protocol != self.current_protocol
            and self.state not in (SwapState.SWITCHING, SwapState.BOOT)
        ):
            logger.info(
                "Firmware reports %s active (was %s) with no command in flight — following it",
                _name(reported_protocol), _name(self.current_protocol),
            )
            self.previous_protocol = self.current_protocol
            self.current_protocol = reported_protocol

        # --- State reactions ------------------------------------------
        if self.state is SwapState.WAIT_LINK and connected:
            if reported_protocol is not None and reported_protocol >= 0:
                self.current_protocol = reported_protocol
                self.previous_protocol = reported_protocol
            self._enter(SwapState.READY_DEFAULT, "link up on the boot protocol, Wi-Fi is the first target")
            return

        if not connected and self.state in (SwapState.STABLE, SwapState.READY_DEFAULT):
            # The nodes own the 5-second recovery window; the UNO Q's job is
            # simply to stop commanding until it resolves.
            self.recovery_started_at = now
            self.recovery_protocol = self.current_protocol
            self._enter(SwapState.RECOVERING, f"{_name(self.current_protocol)} link lost, nodes are recovering")
            return

        if connected and self.state is SwapState.RECOVERING:
            self.recovery_started_at = 0.0
            self.recovery_protocol = None
            target = SwapState.STABLE if self.default_protocol_commanded else SwapState.READY_DEFAULT
            self._enter(target, "link recovered")

    def on_switch_result(
        self,
        status: str,
        attempted: Optional[int],
        active: Optional[int],
        reason: str = "",
        now: Optional[float] = None,
    ) -> None:
        """§6/§7: Node A's verdict on a commanded switch.

        This is the authoritative end of a transition — success commits the new
        protocol, failure means the nodes have already rolled themselves back
        to previous_protocol, and rejected means the command never took effect.
        """
        now = time.time() if now is None else now
        status = (status or "").strip().lower()
        if self.state is not SwapState.SWITCHING:
            logger.info("switch_result(%s) with no transition in flight — resyncing from it", status)

        if attempted is None:
            attempted = self.requested_protocol if self.requested_protocol is not None else self.current_protocol

        if status == "success":
            new_active = attempted if active is None else active
            if self.state is not SwapState.SWITCHING:
                # Resync: no trial was open locally, so the rollback target was
                # never captured at command time.
                self.previous_protocol = self.current_protocol
            self.current_protocol = new_active
            if self.current_protocol == PROTOCOL_BLE:
                # BLE settling guard starts when the connection is established.
                self.ble_guard_until = now + self.BLE_SETTLE_S
            self._finish_transition("success", attempted, self.current_protocol, now)
            self._enter(SwapState.STABLE, f"{_name(self.current_protocol)} trial succeeded")
        elif status == "failed":
            # The nodes have already reverted to previous_protocol themselves.
            rolled_back_to = active if active is not None else self.previous_protocol
            self.current_protocol = rolled_back_to
            self._finish_transition("failed", attempted, rolled_back_to, now)
            self._enter(SwapState.STABLE, f"{_name(attempted)} trial failed, rolled back to {_name(rolled_back_to)}")
        elif status == "rejected":
            # Nothing was torn down; the active protocol is untouched.
            if active is not None:
                self.current_protocol = active
            self._finish_transition("rejected", attempted, self.current_protocol, now, reason=reason)
            self._enter(SwapState.STABLE, f"node refused the switch to {_name(attempted)} ({reason or 'no reason given'})")
        else:
            logger.warning("Unknown switch_result status %r — ignoring", status)

    def _finish_transition(self, outcome: str, attempted: int, resulting: int, now: float, reason: str = "") -> None:
        self.history.append(
            TransitionRecord(
                at=now,
                attempted=attempted,
                outcome=outcome,
                resulting=resulting,
                source=self.transition_source,
                reason=reason,
            )
        )
        del self.history[:-32]
        self.requested_protocol = None
        self.transition_start_time = 0.0
        self.transition_source = ""

    def tick(self, now: Optional[float] = None) -> None:
        """Backstop for a transition whose switch_result never arrived (§5/§7).

        Treated exactly like a reported failure: the nodes' own trial has
        expired by now and they roll back to previousProtocol on their side,
        so the UNO Q's model of the world has to do the same.
        """
        now = time.time() if now is None else now
        if self.state is not SwapState.SWITCHING:
            return
        deadline = self.transition_start_time + self.TRANSITION_TIMEOUT_S + self.RESULT_GRACE_S
        if now < deadline:
            return
        attempted = self.requested_protocol if self.requested_protocol is not None else self.current_protocol
        logger.warning(
            "No switch_result for %s within %.1fs — assuming FAILED, rolling back to %s",
            _name(attempted), self.TRANSITION_TIMEOUT_S + self.RESULT_GRACE_S, _name(self.previous_protocol),
        )
        self.current_protocol = self.previous_protocol
        self._finish_transition("timeout", attempted, self.current_protocol, now)
        self._enter(SwapState.STABLE, f"{_name(attempted)} trial timed out, back on {_name(self.current_protocol)}")

    # ------------------------------------------------------------------
    # The decision gate (§9)
    # ------------------------------------------------------------------

    def evaluate_gate(self, now: Optional[float] = None) -> GateResult:
        """linkConnected == true AND protocolTransitionInProgress == false."""
        now = time.time() if now is None else now
        if not self.both_nodes_initialized:
            missing = "A" if not self.node_a_initialized else "B"
            return GateResult(False, f"node {missing} has not reported initialization")
        if not self.link_connected:
            return GateResult(False, "link is not connected")
        if self.state is SwapState.SWITCHING:
            return GateResult(False, "a protocol transition is already in progress")
        if self.state is SwapState.RECOVERING:
            return GateResult(False, "link recovery in progress")
        if self.state in (SwapState.BOOT, SwapState.WAIT_LINK):
            return GateResult(False, f"state is {self.state.value}")
        if now < self.ble_guard_until:
            return GateResult(
                False,
                f"BLE settling guard active for another {self.ble_guard_until - now:.1f}s",
                deferrable=True,
            )
        return GateResult(True)

    @property
    def transition_in_progress(self) -> bool:
        return self.state is SwapState.SWITCHING

    def request_switch(
        self,
        node: str,
        protocol: int,
        source: str = "operator",
        now: Optional[float] = None,
    ) -> Tuple[Verdict, str]:
        """Ask to switch. Returns (verdict, reason).

        SEND means the caller must transmit the command and then call
        on_command_sent(). DEFERRED means it is parked in the pending slot for
        take_pending() to release once the BLE guard elapses.
        """
        now = time.time() if now is None else now
        gate = self.evaluate_gate(now)
        if gate.allowed:
            return Verdict.SEND, ""
        if gate.deferrable:
            if self.pending is not None:
                logger.info(
                    "Replacing pending %s command with %s (BLE guard still active)",
                    _name(self.pending.protocol), _name(protocol),
                )
            self.pending = PendingCommand(node=node, protocol=protocol, source=source, queued_at=now)
            logger.info("Holding switch to %s until the BLE settling guard clears", _name(protocol))
            return Verdict.DEFERRED, gate.reason
        return Verdict.REJECTED, gate.reason

    def take_pending(self, now: Optional[float] = None) -> Optional[PendingCommand]:
        """Release a held command once the gate reopens, or drop it if the
        system is no longer in a state where executing it makes sense."""
        now = time.time() if now is None else now
        if self.pending is None:
            return None
        gate = self.evaluate_gate(now)
        if gate.deferrable:
            return None  # still guarded, keep holding
        if not gate.allowed:
            if self.state in (SwapState.SWITCHING,):
                return None  # transient, keep holding
            logger.info("Dropping pending %s command: %s", _name(self.pending.protocol), gate.reason)
            self.pending = None
            return None
        command = self.pending
        self.pending = None
        return command

    def on_command_sent(self, protocol: int, source: str = "operator", now: Optional[float] = None) -> None:
        """§5: a switch command has gone out — open the trial window."""
        now = time.time() if now is None else now
        # §5: "The current protocol becomes previousProtocol. The requested
        # protocol becomes trialProtocol." Capturing the rollback target here
        # rather than on success is what makes a timed-out trial roll back to
        # the protocol that was actually running, instead of to whatever was
        # running one switch earlier.
        self.previous_protocol = self.current_protocol
        self.requested_protocol = protocol
        self.transition_start_time = now
        self.transition_source = source
        if protocol == self.DEFAULT_PROTOCOL and not self.default_protocol_commanded:
            self.default_protocol_commanded = True
        if source.startswith("operator"):
            self.operator_hold_until = now + self.OPERATOR_HOLD_S
        self._enter(
            SwapState.SWITCHING,
            f"trialing {_name(protocol)} for {self.TRANSITION_TIMEOUT_S:.0f}s (from {_name(self.current_protocol)})",
        )

    # ------------------------------------------------------------------
    # Autonomous actions the ingest loop asks about
    # ------------------------------------------------------------------

    def needs_default_protocol(self, now: Optional[float] = None) -> bool:
        """§3: the first operating protocol after initialization is Wi-Fi.

        True exactly once, when the link has come up on the boot protocol and
        Wi-Fi has not yet been commanded.
        """
        if self.default_protocol_commanded:
            return False
        if self.state is not SwapState.READY_DEFAULT:
            return False
        if self.current_protocol == self.DEFAULT_PROTOCOL:
            # Already there (e.g. the nodes came up on Wi-Fi) — nothing to command.
            self.default_protocol_commanded = True
            self._enter(SwapState.STABLE, "already on the default protocol")
            return False
        return self.evaluate_gate(now).allowed

    def ready_for_adaptive(self, now: Optional[float] = None) -> bool:
        """§11: adaptive decisions only after the first Wi-Fi transition, and
        only through the same gate."""
        now = time.time() if now is None else now
        if not self.default_protocol_commanded:
            return False
        if self.state is not SwapState.STABLE:
            return False
        if now < self.operator_hold_until:
            return False
        return self.evaluate_gate(now).allowed

    # ------------------------------------------------------------------

    def _enter(self, state: SwapState, reason: str) -> None:
        if self.state is not state:
            logger.info("STATE %s -> %s (%s)", self.state.value, state.value, reason)
        self.last_reason = reason
        self.state = state

    def snapshot(self, now: Optional[float] = None) -> Dict[str, object]:
        now = time.time() if now is None else now
        gate = self.evaluate_gate(now)
        return {
            "state": self.state.value,
            "state_reason": self.last_reason,
            "node_a_initialized": self.node_a_initialized,
            "node_b_initialized": self.node_b_initialized,
            "link_connected": self.link_connected,
            "current_protocol": self.current_protocol,
            "current_protocol_name": _name(self.current_protocol),
            "previous_protocol": self.previous_protocol,
            "previous_protocol_name": _name(self.previous_protocol),
            "requested_protocol": self.requested_protocol,
            "requested_protocol_name": _name(self.requested_protocol) if self.requested_protocol is not None else None,
            "transition_in_progress": self.transition_in_progress,
            "transition_elapsed_s": round(now - self.transition_start_time, 2) if self.transition_in_progress else None,
            "transition_timeout_s": self.TRANSITION_TIMEOUT_S,
            "default_protocol_commanded": self.default_protocol_commanded,
            "ble_guard_remaining_s": round(max(0.0, self.ble_guard_until - now), 2),
            "operator_hold_remaining_s": round(max(0.0, self.operator_hold_until - now), 2),
            "recovery_elapsed_s": round(now - self.recovery_started_at, 2) if self.state is SwapState.RECOVERING else None,
            "switch_allowed": gate.allowed,
            "switch_blocked_reason": None if gate.allowed else gate.reason,
            "pending_command": None
            if self.pending is None
            else {
                "node": self.pending.node,
                "protocol": self.pending.protocol,
                "protocol_name": _name(self.pending.protocol),
                "source": self.pending.source,
                "waiting_s": round(now - self.pending.queued_at, 2),
            },
            "recent_transitions": [
                {
                    "at": round(item.at, 3),
                    "attempted": item.attempted,
                    "attempted_name": _name(item.attempted),
                    "outcome": item.outcome,
                    "resulting": item.resulting,
                    "resulting_name": _name(item.resulting),
                    "source": item.source,
                    "reason": item.reason,
                }
                for item in reversed(self.history[-10:])
            ],
        }


def _name(protocol: Optional[int]) -> str:
    if protocol is None:
        return "NONE"
    return PROTOCOL_NAMES.get(protocol, "UNKNOWN")
