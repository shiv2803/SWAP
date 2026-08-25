import asyncio
import contextlib
import importlib
import json
import logging
import math
import os
import time
from collections import deque
from dataclasses import asdict
from typing import Any, AsyncIterator, Deque, Dict, List, Optional, Union

from fastapi import FastAPI, HTTPException, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import RedirectResponse, StreamingResponse
from pydantic import BaseModel, Field

from .common import PROTOCOL_NAME_TO_ID, PROTOCOL_NAMES, TelemetryRecord
from .control import SwapControlMachine, Verdict
from .link_quality_model import Decision, LinkQualityModel
from .telemetry_link import create_source_from_env

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("swap.app")

app = FastAPI(title="SWAP Prototype API", version="1.0.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

telemetry_queue: "asyncio.Queue[Union[TelemetryRecord, dict]]" = asyncio.Queue()
telemetry_source = create_source_from_env()
command_sender = telemetry_source.command_sender()
model = LinkQualityModel()

RECENT_HISTORY_LEN = 500
recent_records: Deque[TelemetryRecord] = deque(maxlen=RECENT_HISTORY_LEN)
latest_record_by_node: Dict[str, TelemetryRecord] = {}
latest_decision_by_node: Dict[str, Decision] = {}
ingest_task: Optional[asyncio.Task] = None
# Set when the telemetry source task dies (e.g. the 'arduino' module is missing
# because the backend was started off-board). Without this the server stayed up
# and simply never produced a frame, which looks identical to a dead radio link.
ingest_error: Optional[str] = None

# Seconds between keep-alive frames on an idle live stream. Browsers and any
# NAT/router in between happily drop a TCP connection that has been silent for
# a minute; a periodic ping keeps /ws/live and /events open when telemetry is
# sparse, and makes a half-open socket fail fast instead of looking "connected".
HEARTBEAT_SECONDS = 10.0
# Per-client backlog. A stalled reader must never block the ingest loop, so a
# full queue drops its oldest frame rather than applying backpressure.
CLIENT_QUEUE_MAXLEN = 100


class Broadcaster:
    """Fan-out hub shared by the WebSocket and SSE live streams.

    Each subscriber gets its own bounded queue, so publishing is non-blocking:
    the ingest loop never awaits a slow or dead client (the previous version
    awaited send_json() per client inline, which stalled ingest whenever one
    browser tab went unresponsive).
    """

    def __init__(self, maxsize: int = CLIENT_QUEUE_MAXLEN) -> None:
        self._subscribers: List["asyncio.Queue[dict]"] = []
        self._maxsize = maxsize

    def subscribe(self) -> "asyncio.Queue[dict]":
        queue: "asyncio.Queue[dict]" = asyncio.Queue(maxsize=self._maxsize)
        self._subscribers.append(queue)
        return queue

    def unsubscribe(self, queue: "asyncio.Queue[dict]") -> None:
        if queue in self._subscribers:
            self._subscribers.remove(queue)

    def publish(self, payload: dict) -> None:
        for queue in list(self._subscribers):
            if queue.full():
                with contextlib.suppress(asyncio.QueueEmpty):
                    queue.get_nowait()
            with contextlib.suppress(asyncio.QueueFull):
                queue.put_nowait(payload)

    @property
    def count(self) -> int:
        return len(self._subscribers)


broadcaster = Broadcaster()


def _json_safe(value: Any) -> Any:
    """Strip NaN/Infinity, which json.dumps emits verbatim and JSON.parse rejects.

    A single non-finite float (an unmeasured SNR, a divide-by-zero throughput)
    used to poison the whole frame: the browser threw on JSON.parse and the
    dashboard silently stopped updating.
    """
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {k: _json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(v) for v in value]
    return value


def _websocket_library() -> Optional[str]:
    for name in ("websockets", "wsproto"):
        try:
            importlib.import_module(name)
            return name
        except ImportError:
            continue
    return None


# ============================================================
# SWAP CONTROL LOGIC
# ============================================================
# The state machine itself lives in control.py -- the specification requires
# an explicit machine rather than if/else scattered through the request
# handlers, so everything about initialization gating, the 5-second trial,
# rollback, the BLE settling guard and link recovery is in one place.

control = SwapControlMachine()

# §11: after the first Wi-Fi transition the UNO Q runs the adaptive loop on
# its own. Off by default only if explicitly disabled, since "the UNO Q
# decides when a protocol switch should occur" is the whole point.
auto_switch_enabled = os.environ.get("SWAP_AUTO_SWITCH", "1").strip().lower() not in ("0", "false", "no")


# ============================================================
# API MODELS
# ============================================================

class ForceProtocolRequest(BaseModel):
    node: str = Field(pattern="^(a|b)$")
    protocol: int = Field(ge=0, le=2)


# ============================================================
# STARTUP / SHUTDOWN
# ============================================================

@app.on_event("startup")
async def on_startup() -> None:
    global ingest_task
    ws_lib = _websocket_library()
    if ws_lib is None:
        # Without one of these, uvicorn answers the /ws/live upgrade with a
        # plain HTTP error and the browser reports "WebSocket is closed before
        # the connection is established" -- the connection never gets made.
        logger.warning(
            "No WebSocket library installed (websockets / wsproto). /ws/live upgrades "
            "will be REJECTED by uvicorn. Fix: pip install 'uvicorn[standard]' websockets. "
            "The /events SSE stream works regardless and the frontend falls back to it."
        )
    else:
        logger.info("WebSocket support via %s; /ws/live is available", ws_lib)
    ingest_task = asyncio.create_task(_ingest_loop())


@app.on_event("shutdown")
async def on_shutdown() -> None:
    if ingest_task is not None:
        ingest_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await ingest_task


# ============================================================
# ROUTES
# ============================================================

@app.get("/", include_in_schema=False)
def read_root():
    return RedirectResponse(url="/docs")


@app.get("/health")
def health() -> Dict[str, str]:
    return {"status": "ok"}


@app.get("/status")
def get_status() -> Dict[str, object]:
    data = {}
    for node in ("a", "b"):
        record = latest_record_by_node.get(node)
        decision = latest_decision_by_node.get(node) or model.recommend_for_node(node)
        data[node] = _format_status(record=record, decision=decision)
    ingest = dict(telemetry_source.counters())
    ingest["source_error"] = ingest_error
    return {
        "nodes": data,
        "ingest": ingest,
        "state_machine": control.snapshot(),
        "auto_switch": auto_switch_enabled,
    }


@app.get("/telemetry/recent")
def get_recent(limit: int = Query(default=50, ge=1, le=500)) -> Dict[str, object]:
    payload = [asdict(record) for record in list(recent_records)[-limit:]]
    return {"count": len(payload), "records": payload}


@app.get("/model/info")
def get_model_info() -> Dict[str, object]:
    return model.model_info()


class UartRawSendRequest(BaseModel):
    message: str


@app.post("/debug/uart/send")
async def debug_uart_send(request: UartRawSendRequest) -> Dict[str, object]:
    if os.environ.get("SWAP_INPUT_MODE", "sim").strip().lower() != "serial":
        raise HTTPException(status_code=409, detail="Only available in SWAP_INPUT_MODE=serial")
    from arduino.app_utils import Bridge
    loop = asyncio.get_running_loop()
    await loop.run_in_executor(None, lambda: Bridge.call("uart_send_raw", request.message, timeout=5))
    return {"ok": True, "sent": request.message}


@app.post("/decide")
async def decide(node: str = Query(default="a", pattern="^(a|b)$")) -> Dict[str, object]:
    status = latest_record_by_node.get(node)
    if status is None:
        raise HTTPException(status_code=503, detail=f"No telemetry received yet for node {node}")

    decision = model.recommend_for_node(node)
    if decision is None:
        raise HTTPException(status_code=503, detail="Not enough telemetry samples yet")

    forced = False
    deferred = False
    if decision.protocol != control.current_protocol:
        verdict, reason = control.request_switch(node=node, protocol=decision.protocol, source="decide")
        if verdict is Verdict.REJECTED:
            raise HTTPException(status_code=409, detail=f"Switch blocked: {reason}")
        if verdict is Verdict.DEFERRED:
            # Held by the BLE settling guard, not discarded -- the ingest loop
            # sends it as soon as the guard elapses.
            deferred = True
        else:
            await _send_switch(node=node, protocol=decision.protocol, source="decide")
            forced = True

    latest_decision_by_node[node] = decision

    return {
        "node": node,
        "deferred": deferred,
        "state": control.state.value,
        "current_protocol": status.active_protocol,
        "recommended_protocol": decision.protocol,
        "recommended_protocol_name": PROTOCOL_NAMES[decision.protocol],
        "confidence": decision.confidence,
        "decision_source": decision.source,
        "forced": forced,
        "raw_scores": decision.raw_scores,
        "probabilities": decision.probabilities,
        "lqi": decision.lqi,
        "hysteresis_guarded": decision.hysteresis_guarded,
        "dwell_guarded": decision.dwell_guarded,
    }


@app.post("/force")
async def force_protocol(request: ForceProtocolRequest) -> Dict[str, object]:
    """Operator override. Goes through exactly the same gate as an adaptive
    decision -- §9 admits no exceptions for manual commands."""
    verdict, reason = control.request_switch(node=request.node, protocol=request.protocol, source="operator")
    if verdict is Verdict.REJECTED:
        raise HTTPException(status_code=409, detail=f"Switch blocked: {reason}")

    if verdict is Verdict.DEFERRED:
        return {
            "ok": True,
            "deferred": True,
            "reason": reason,
            "node": request.node,
            "protocol": request.protocol,
            "protocol_name": PROTOCOL_NAMES[request.protocol],
            "state": control.state.value,
        }

    await _send_switch(node=request.node, protocol=request.protocol, source="operator")
    return {
        "ok": True,
        "deferred": False,
        "node": request.node,
        "protocol": request.protocol,
        "protocol_name": PROTOCOL_NAMES[request.protocol],
        "state": control.state.value,
    }


class AutoSwitchRequest(BaseModel):
    enabled: bool


@app.get("/control")
def get_control() -> Dict[str, object]:
    """The full control-logic state: which spec state, what the gate says,
    what is pending, and the recent transition outcomes."""
    return {"auto_switch": auto_switch_enabled, **control.snapshot()}


@app.post("/control/auto")
def set_auto_switch(request: AutoSwitchRequest) -> Dict[str, object]:
    """Turn the autonomous adaptive layer (§11) on or off.

    Off leaves initialization gating, the first Wi-Fi transition, trials and
    rollback intact -- it only stops the UNO Q from choosing switches by
    itself, so an operator can drive it manually via /force.
    """
    global auto_switch_enabled
    auto_switch_enabled = request.enabled
    logger.info("Adaptive auto-switching %s", "ENABLED" if request.enabled else "DISABLED")
    return {"auto_switch": auto_switch_enabled}


def _hello_frame() -> dict:
    """First frame on any live stream: the current snapshot.

    Lets a client render immediately instead of sitting blank until the next
    telemetry frame arrives, and proves the stream is genuinely established.
    """
    return _json_safe({"type": "hello", "snapshot": get_status()})


async def _drain_incoming(websocket: WebSocket) -> None:
    """Consume client->server frames; returns when the peer goes away.

    receive() (not receive_text()) so a client sending binary or a close frame
    is handled instead of raising inside the send path.
    """
    while True:
        message = await websocket.receive()
        if message.get("type") == "websocket.disconnect":
            return


@app.websocket("/ws/live")
async def live_stream(websocket: WebSocket) -> None:
    await websocket.accept()
    queue = broadcaster.subscribe()
    logger.info("WS client connected (%d live subscriber(s))", broadcaster.count)
    reader: Optional[asyncio.Task] = None
    try:
        await websocket.send_json(_hello_frame())
        reader = asyncio.create_task(_drain_incoming(websocket))
        while not reader.done():
            try:
                payload = await asyncio.wait_for(queue.get(), timeout=HEARTBEAT_SECONDS)
            except asyncio.TimeoutError:
                payload = {"type": "ping", "ts": time.time()}
            await websocket.send_json(payload)
    except (WebSocketDisconnect, asyncio.CancelledError):
        pass
    except Exception as exc:  # noqa: BLE001 - one bad client must not kill ingest
        logger.info("WS client dropped: %s", exc)
    finally:
        if reader is not None:
            reader.cancel()
            with contextlib.suppress(Exception):
                await reader
        broadcaster.unsubscribe(queue)
        with contextlib.suppress(Exception):
            await websocket.close()
        logger.info("WS client disconnected (%d live subscriber(s))", broadcaster.count)


def _sse_frame(payload: dict) -> str:
    return f"data: {json.dumps(payload)}\n\n"


@app.get("/events")
async def sse_stream(request: Request) -> StreamingResponse:
    """Server-Sent Events mirror of /ws/live.

    Plain HTTP, so it works when the WebSocket upgrade cannot complete (no
    websockets library in the venv, or a proxy that won't upgrade). The frontend
    falls back to this automatically.
    """
    queue = broadcaster.subscribe()
    logger.info("SSE client connected (%d live subscriber(s))", broadcaster.count)

    async def event_source() -> AsyncIterator[str]:
        try:
            yield _sse_frame(_hello_frame())
            while True:
                if await request.is_disconnected():
                    break
                try:
                    payload = await asyncio.wait_for(queue.get(), timeout=HEARTBEAT_SECONDS)
                except asyncio.TimeoutError:
                    yield ": keep-alive\n\n"
                    continue
                yield _sse_frame(payload)
        finally:
            broadcaster.unsubscribe(queue)
            logger.info("SSE client disconnected (%d live subscriber(s))", broadcaster.count)

    return StreamingResponse(
        event_source(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache, no-store, no-transform",
            "Connection": "keep-alive",
            # Stops nginx-style proxies buffering the stream into silence.
            "X-Accel-Buffering": "no",
        },
    )


# ============================================================
# INGEST LOOP
# ============================================================

def _on_source_task_done(task: "asyncio.Task") -> None:
    """Surface a telemetry source that died instead of waiting forever for it."""
    global ingest_error
    if task.cancelled():
        return
    exc = task.exception()
    if exc is not None:
        ingest_error = f"{type(exc).__name__}: {exc}"
        logger.error("Telemetry source stopped: %s", ingest_error)


def _protocol_id(value: object) -> Optional[int]:
    """Firmware reports protocols by name ("wifi"), the API by id (0)."""
    if value is None:
        return None
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value if value in PROTOCOL_NAMES else None
    return PROTOCOL_NAME_TO_ID.get(str(value).strip().lower())


# ============================================================
# COMMAND PATH (§4, §8, §9)
# ============================================================

async def _send_switch(node: str, protocol: int, source: str) -> None:
    """Transmit a switch command and open the trial window.

    Node A propagates it to Node B; both then run the 5-second trial and
    Node A reports the outcome as a switch_result frame. The UNO Q never
    assumes the requested protocol is active just because it sent the
    command (§4) -- current_protocol only moves on that report.
    """
    await command_sender.force_protocol(node=node, protocol=protocol)
    control.on_command_sent(protocol=protocol, source=source)
    logger.info("Sent switch command: node=%s -> %s (%s)", node, PROTOCOL_NAMES[protocol], source)


async def _drive_control() -> None:
    """One pass of the UNO Q's own decision-making, run per telemetry frame.

    Ordering matters: a command already held by the BLE guard outranks a new
    one, the mandatory first Wi-Fi transition outranks adaptive choices, and
    the adaptive layer only ever runs once the system is stable.
    """
    now = time.time()
    control.tick(now)

    pending = control.take_pending(now)
    if pending is not None:
        await _send_switch(pending.node, pending.protocol, f"{pending.source}:deferred")
        return

    # §3: the first operating protocol after initialization is always Wi-Fi.
    if control.needs_default_protocol(now):
        await _send_switch("a", control.DEFAULT_PROTOCOL, "default")
        return

    # §11: normal smart-adaptive operation.
    if not auto_switch_enabled or not control.ready_for_adaptive(now):
        return
    decision = latest_decision_by_node.get("a")
    if decision is None or decision.protocol == control.current_protocol:
        return
    verdict, reason = control.request_switch("a", decision.protocol, source="adaptive", now=now)
    if verdict is Verdict.SEND:
        await _send_switch("a", decision.protocol, "adaptive")
    elif verdict is Verdict.REJECTED:
        logger.debug("Adaptive switch to %s blocked: %s", PROTOCOL_NAMES[decision.protocol], reason)


async def _ingest_loop() -> None:
    source_task = asyncio.create_task(telemetry_source.run(telemetry_queue))
    source_task.add_done_callback(_on_source_task_done)
    try:
        while True:
            item = await telemetry_queue.get()
            
            # ---- CONTROL EVENTS FROM NODE A ----
            if isinstance(item, dict):
                kind = item.get("type")
                if kind == "node_initialized":
                    control.on_node_initialized(item["node"])
                elif kind == "switch_result":
                    control.on_switch_result(
                        status=item.get("status", ""),
                        attempted=_protocol_id(item.get("attempted")),
                        active=_protocol_id(item.get("active")),
                        reason=str(item.get("reason", "")),
                    )
                    await _broadcast_ws(record=None, decision=None, event=item)
                else:
                    logger.debug("Ignoring unknown control event: %s", item)
                await _drive_control()
                continue

            # ---- NORMAL TELEMETRY RECORD ----
            record = item
            recent_records.append(record)
            latest_record_by_node[record.node] = record

            # §2: connectivity is Node A's report, not something inferred from
            # "some protocol is active" -- that inference was always true and
            # left the switching gate permanently open. Frames from firmware
            # too old to carry the field fall back to the old assumption.
            if record.node == "a":
                connected = (
                    record.link_connected
                    if record.link_connected is not None
                    else record.active_protocol != -1
                )
                control.on_link_report(connected=connected, reported_protocol=record.active_protocol)

            decision = model.observe(record)
            if decision is not None:
                latest_decision_by_node[record.node] = decision

            await _drive_control()
            await _broadcast_ws(record=record, decision=decision)
    finally:
        source_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await source_task


async def _broadcast_ws(
    record: Optional[TelemetryRecord],
    decision: Optional[Decision],
    event: Optional[dict] = None,
) -> None:
    """Push one live frame to every WebSocket/SSE subscriber.

    Carries the control snapshot alongside the telemetry so a dashboard can
    show *why* switching is or isn't happening (which state, which gate) as
    well as the raw readings.
    """
    if broadcaster.count == 0:
        return
    payload = {
        "type": "telemetry" if event is None else "control_event",
        "record": None if record is None else asdict(record),
        "decision": None
        if decision is None
        else {
            "protocol": decision.protocol,
            "protocol_name": PROTOCOL_NAMES[decision.protocol],
            "confidence": decision.confidence,
            "source": decision.source,
            "raw_scores": decision.raw_scores,
            "probabilities": decision.probabilities,
            "lqi": decision.lqi,
            "hysteresis_guarded": decision.hysteresis_guarded,
            "dwell_guarded": decision.dwell_guarded,
            "ble_deprioritized": decision.ble_deprioritized,
            "best_candidate": decision.best_candidate,
        },
        "control": control.snapshot(),
        # So a dashboard toggle reflects what the backend is actually doing
        # rather than its own local guess.
        "auto_switch": auto_switch_enabled,
    }
    if event is not None:
        payload["event"] = event
    broadcaster.publish(_json_safe(payload))


def _format_status(record: Optional[TelemetryRecord], decision: Optional[Decision]) -> Optional[Dict[str, object]]:
    if record is None:
        return None
    if decision is None:
        recommended_protocol = None
        recommended_protocol_name = None
        confidence = None
        decision_source = None
        extra: Dict[str, object] = {}
    else:
        recommended_protocol = decision.protocol
        recommended_protocol_name = PROTOCOL_NAMES[decision.protocol]
        confidence = decision.confidence
        decision_source = decision.source
        extra = {
            "raw_scores": decision.raw_scores,
            "probabilities": decision.probabilities,
            "lqi": decision.lqi,
            "hysteresis_guarded": decision.hysteresis_guarded,
            "dwell_guarded": decision.dwell_guarded,
            "ble_deprioritized": decision.ble_deprioritized,
        }

    return {
        "active_protocol_reported": record.active_protocol,
        "active_protocol_name": PROTOCOL_NAMES.get(record.active_protocol, "UNKNOWN"),
        "recommended_protocol": recommended_protocol,
        "recommended_protocol_name": recommended_protocol_name,
        "confidence": confidence,
        "decision_source": decision_source,
        "wifi_rssi": record.wifi_rssi,
        "wifi_loss": record.wifi_loss,
        "ble_rssi": record.ble_rssi,
        "lora_rssi": record.lora_rssi,
        "lora_snr": record.lora_snr,
        "lora_loss": record.lora_loss,
        "rtt_ms": record.rtt_ms,
        "recv_ts": record.recv_ts,
        "crc_ok": record.crc_ok,
        "time_on_air_ms": record.time_on_air_ms,
        "tx_power_dbm": record.tx_power_dbm,
        "channel": record.channel,
        "spreading_factor": record.spreading_factor,
        "bandwidth_khz": record.bandwidth_khz,
        "coding_rate": record.coding_rate,
        "packet_loss": record.packet_loss,
        "packet_success_rate": record.packet_success_rate,
        "latency_ms": record.latency_ms,
        "max_rtt_ms": record.max_rtt_ms,
        "jitter_ms": record.jitter_ms,
        "throughput_bps": record.throughput_bps,
        "stability_db": record.stability_db,
        "probes_sent": record.probes_sent,
        "probes_acked": record.probes_acked,
        **extra,
    }