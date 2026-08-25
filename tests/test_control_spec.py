"""Conformance tests for SWAP_UNO_Q_Control_Logic_Specification.md.

Each test names the spec section it pins down. Runs under pytest, or directly
with `python tests/test_control_spec.py` (no pytest required) so it can be run
on the UNO Q itself, where the venv is deliberately minimal.

Time is injected everywhere (`now=`), so nothing here sleeps.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from swap_backend.common import PROTOCOL_BLE, PROTOCOL_LORA, PROTOCOL_WIFI  # noqa: E402
from swap_backend.control import SwapControlMachine, SwapState, Verdict  # noqa: E402
from swap_backend.link_quality_model import (  # noqa: E402
    BLE_PREFERENCE_MARGIN,
    _apply_ble_priority,
)

T0 = 1_000.0


def _booted(now=T0):
    """A machine that has both node_initialized events but no link yet."""
    machine = SwapControlMachine()
    machine.on_node_initialized("a")
    machine.on_node_initialized("b")
    return machine


def _linked(now=T0):
    """Both nodes initialized and the boot LoRa link reported CONNECTED."""
    machine = _booted(now)
    machine.on_link_report(connected=True, reported_protocol=PROTOCOL_LORA, now=now)
    return machine


def _on_wifi(now=T0):
    """Past the mandatory first Wi-Fi transition, sitting in STABLE."""
    machine = _linked(now)
    machine.on_command_sent(PROTOCOL_WIFI, source="default", now=now)
    machine.on_switch_result("success", PROTOCOL_WIFI, PROTOCOL_WIFI, now=now + 1.0)
    return machine


# ---------------------------------------------------------------- §0 boot gate

def test_no_switching_before_both_nodes_initialize():
    machine = SwapControlMachine()
    assert machine.state is SwapState.BOOT
    assert not machine.evaluate_gate(T0).allowed

    machine.on_node_initialized("a")
    # Node A alone is not enough: the spec keeps independent flags and requires
    # both before the sequence may advance.
    assert machine.state is SwapState.BOOT
    assert not machine.evaluate_gate(T0).allowed

    machine.on_node_initialized("b")
    assert machine.state is SwapState.WAIT_LINK


def test_initialized_is_not_connected():
    """§0: 'A node can be initialized while the link is still disconnected.'"""
    machine = _booted()
    gate = machine.evaluate_gate(T0)
    assert not gate.allowed
    assert "link" in gate.reason


# ------------------------------------------------------- §2/§3 link then Wi-Fi

def test_link_connected_opens_the_gate_and_targets_wifi():
    machine = _linked()
    assert machine.state is SwapState.READY_DEFAULT
    assert machine.evaluate_gate(T0).allowed
    # §3: BOOT -> LoRa -> verify communication -> switch to Wi-Fi.
    assert machine.current_protocol == PROTOCOL_LORA
    assert machine.needs_default_protocol(T0)
    assert machine.DEFAULT_PROTOCOL == PROTOCOL_WIFI


def test_default_protocol_is_requested_only_once():
    machine = _linked()
    machine.on_command_sent(PROTOCOL_WIFI, source="default", now=T0)
    machine.on_switch_result("success", PROTOCOL_WIFI, PROTOCOL_WIFI, now=T0 + 1.0)
    assert not machine.needs_default_protocol(T0 + 2.0)


# ------------------------------------------------------ §5/§6 trial and commit

def test_successful_trial_commits_and_records_previous():
    machine = _linked()
    machine.on_command_sent(PROTOCOL_WIFI, source="default", now=T0)
    assert machine.state is SwapState.SWITCHING
    assert machine.requested_protocol == PROTOCOL_WIFI

    machine.on_switch_result("success", PROTOCOL_WIFI, PROTOCOL_WIFI, now=T0 + 1.2)
    assert machine.state is SwapState.STABLE
    assert machine.current_protocol == PROTOCOL_WIFI
    assert machine.previous_protocol == PROTOCOL_LORA
    assert machine.requested_protocol is None


def test_uno_q_does_not_assume_the_command_took_effect():
    """§4: sending the command must not move the active protocol."""
    machine = _linked()
    machine.on_command_sent(PROTOCOL_WIFI, source="default", now=T0)
    assert machine.current_protocol == PROTOCOL_LORA


# ----------------------------------------------------------------- §8 lockout

def test_no_second_command_during_a_trial():
    machine = _linked()
    machine.on_command_sent(PROTOCOL_WIFI, source="default", now=T0)
    verdict, reason = machine.request_switch("a", PROTOCOL_BLE, now=T0 + 0.5)
    assert verdict is Verdict.REJECTED
    assert "in progress" in reason
    # And it is not silently queued either -- a rejected command is refused.
    assert machine.pending is None


# ------------------------------------------------------- §7 rollback semantics

def test_failed_trial_rolls_back_to_previous_not_lora():
    """§7 example 2: active Wi-Fi, BLE fails -> rollback to Wi-Fi."""
    machine = _on_wifi()
    machine.on_command_sent(PROTOCOL_BLE, source="adaptive", now=T0 + 10.0)
    machine.on_switch_result("failed", PROTOCOL_BLE, PROTOCOL_WIFI, now=T0 + 15.0)
    assert machine.current_protocol == PROTOCOL_WIFI
    assert machine.state is SwapState.STABLE
    assert machine.history[-1].outcome == "failed"


def test_missing_switch_result_times_out_into_a_rollback():
    machine = _on_wifi()
    start = T0 + 10.0
    machine.on_command_sent(PROTOCOL_BLE, source="adaptive", now=start)
    # Still inside the trial window plus the reporting grace: no verdict yet.
    machine.tick(start + machine.TRANSITION_TIMEOUT_S)
    assert machine.state is SwapState.SWITCHING

    machine.tick(start + machine.TRANSITION_TIMEOUT_S + machine.RESULT_GRACE_S + 0.1)
    assert machine.state is SwapState.STABLE
    assert machine.current_protocol == PROTOCOL_WIFI
    assert machine.history[-1].outcome == "timeout"


def test_rejected_switch_leaves_the_active_protocol_untouched():
    machine = _on_wifi()
    machine.on_command_sent(PROTOCOL_BLE, source="operator", now=T0 + 10.0)
    machine.on_switch_result(
        "rejected", PROTOCOL_BLE, PROTOCOL_WIFI, reason="switch_in_progress", now=T0 + 10.3
    )
    assert machine.current_protocol == PROTOCOL_WIFI
    assert machine.history[-1].outcome == "rejected"
    assert machine.history[-1].reason == "switch_in_progress"


# ---------------------------------------------------- BLE guard (3s, pending)

def test_ble_guard_defers_rather_than_discards():
    machine = _on_wifi()
    machine.on_command_sent(PROTOCOL_BLE, source="adaptive", now=T0 + 10.0)
    machine.on_switch_result("success", PROTOCOL_BLE, PROTOCOL_BLE, now=T0 + 11.0)
    assert machine.current_protocol == PROTOCOL_BLE

    # Inside the 3-second settling guard.
    verdict, _ = machine.request_switch("a", PROTOCOL_WIFI, source="operator", now=T0 + 12.0)
    assert verdict is Verdict.DEFERRED
    assert machine.pending is not None
    assert machine.take_pending(T0 + 12.5) is None  # still guarded

    # After it elapses the held command is released, not dropped.
    released = machine.take_pending(T0 + 14.5)
    assert released is not None
    assert released.protocol == PROTOCOL_WIFI
    assert machine.pending is None


def test_pending_command_is_dropped_if_the_link_dies_first():
    machine = _on_wifi()
    machine.on_command_sent(PROTOCOL_BLE, source="adaptive", now=T0 + 10.0)
    machine.on_switch_result("success", PROTOCOL_BLE, PROTOCOL_BLE, now=T0 + 11.0)
    machine.request_switch("a", PROTOCOL_WIFI, source="operator", now=T0 + 12.0)

    # "execute after the 3-second interval IF the system is still stable and
    # connected" -- it isn't.
    machine.on_link_report(connected=False, reported_protocol=PROTOCOL_BLE, now=T0 + 13.0)
    assert machine.state is SwapState.RECOVERING
    assert machine.take_pending(T0 + 14.5) is None
    assert machine.pending is None


# --------------------------------------------------------- link-loss recovery

def test_link_loss_enters_recovery_and_blocks_commands():
    machine = _on_wifi()
    machine.on_link_report(connected=False, reported_protocol=PROTOCOL_WIFI, now=T0 + 20.0)
    assert machine.state is SwapState.RECOVERING

    gate = machine.evaluate_gate(T0 + 21.0)
    assert not gate.allowed
    verdict, _ = machine.request_switch("a", PROTOCOL_LORA, now=T0 + 21.0)
    assert verdict is Verdict.REJECTED

    machine.on_link_report(connected=True, reported_protocol=PROTOCOL_WIFI, now=T0 + 23.0)
    assert machine.state is SwapState.STABLE
    assert machine.evaluate_gate(T0 + 23.0).allowed


def test_machine_follows_a_firmware_side_rollback():
    """The nodes roll back on their own after a failed recovery; the UNO Q
    must adopt what they report rather than insist on its own bookkeeping."""
    machine = _on_wifi()
    machine.on_link_report(connected=False, reported_protocol=PROTOCOL_WIFI, now=T0 + 20.0)
    # 5 s later the nodes gave up on Wi-Fi and came back on LoRa.
    machine.on_link_report(connected=True, reported_protocol=PROTOCOL_LORA, now=T0 + 25.5)
    assert machine.current_protocol == PROTOCOL_LORA
    assert machine.previous_protocol == PROTOCOL_WIFI
    assert machine.state is SwapState.STABLE


def test_reported_protocol_is_ignored_mid_trial():
    """While SWITCHING the reported protocol is unproven -- adopting it would
    move current_protocol onto a protocol that has not passed its trial."""
    machine = _on_wifi()
    machine.on_command_sent(PROTOCOL_LORA, source="adaptive", now=T0 + 10.0)
    # §5: the rollback target is the protocol that was active when the command
    # was accepted, captured at that moment.
    assert machine.previous_protocol == PROTOCOL_WIFI
    machine.on_link_report(connected=True, reported_protocol=PROTOCOL_LORA, now=T0 + 10.5)
    assert machine.current_protocol == PROTOCOL_WIFI
    assert machine.previous_protocol == PROTOCOL_WIFI


# ------------------------------------------------------------ §11 adaptive gate

def test_adaptive_phase_only_after_the_first_wifi_transition():
    machine = _linked()
    assert not machine.ready_for_adaptive(T0)      # default not commanded yet
    machine.on_command_sent(PROTOCOL_WIFI, source="default", now=T0)
    assert not machine.ready_for_adaptive(T0)      # mid-trial
    machine.on_switch_result("success", PROTOCOL_WIFI, PROTOCOL_WIFI, now=T0 + 1.0)
    assert machine.ready_for_adaptive(T0 + 1.0)


def test_operator_override_is_not_immediately_reversed():
    """Not a spec rule: §11 running autonomously would otherwise undo a manual
    choice on the very next evaluation, making /force pointless."""
    machine = _on_wifi()
    machine.on_command_sent(PROTOCOL_LORA, source="operator", now=T0 + 10.0)
    machine.on_switch_result("success", PROTOCOL_LORA, PROTOCOL_LORA, now=T0 + 11.0)
    assert not machine.ready_for_adaptive(T0 + 12.0)
    assert machine.ready_for_adaptive(T0 + 10.0 + machine.OPERATOR_HOLD_S + 0.1)


def test_operator_hold_does_not_block_the_gate_for_manual_commands():
    """The hold stands the adaptive layer down; it must not stop the operator
    from issuing another command."""
    machine = _on_wifi()
    machine.on_command_sent(PROTOCOL_LORA, source="operator", now=T0 + 10.0)
    machine.on_switch_result("success", PROTOCOL_LORA, PROTOCOL_LORA, now=T0 + 11.0)
    verdict, _ = machine.request_switch("a", PROTOCOL_WIFI, source="operator", now=T0 + 12.0)
    assert verdict is Verdict.SEND


# -------------------------------------------------------------- BLE last policy

def test_ble_needs_a_clear_margin_to_be_selected():
    scores = {PROTOCOL_WIFI: -70.0, PROTOCOL_BLE: -68.0, PROTOCOL_LORA: -95.0}
    protocol, deprioritized = _apply_ble_priority(PROTOCOL_BLE, scores)
    assert protocol == PROTOCOL_WIFI  # 2.0 lead is not "explicitly justified"
    assert deprioritized

    scores[PROTOCOL_BLE] = -70.0 + BLE_PREFERENCE_MARGIN + 1.0
    protocol, deprioritized = _apply_ble_priority(PROTOCOL_BLE, scores)
    assert protocol == PROTOCOL_BLE
    assert not deprioritized


def test_ble_policy_leaves_other_candidates_alone():
    scores = {PROTOCOL_WIFI: -70.0, PROTOCOL_BLE: -60.0, PROTOCOL_LORA: -95.0}
    assert _apply_ble_priority(PROTOCOL_WIFI, scores) == (PROTOCOL_WIFI, False)


def test_deprioritized_ble_falls_back_to_the_better_alternative():
    # LoRa beats Wi-Fi here, so demoting BLE must land on LoRa, not blindly Wi-Fi.
    scores = {PROTOCOL_WIFI: -110.0, PROTOCOL_BLE: -80.0, PROTOCOL_LORA: -78.0}
    protocol, deprioritized = _apply_ble_priority(PROTOCOL_BLE, scores)
    assert protocol == PROTOCOL_LORA
    assert deprioritized


def _main() -> int:
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]
    failures = []
    for name, fn in tests:
        try:
            fn()
            print(f"  PASS  {name}")
        except AssertionError as exc:
            failures.append((name, exc))
            print(f"  FAIL  {name}: {exc}")
        except Exception as exc:  # noqa: BLE001
            failures.append((name, exc))
            print(f"  ERROR {name}: {type(exc).__name__}: {exc}")
    print(f"\n{len(tests) - len(failures)}/{len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(_main())
