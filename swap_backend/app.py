import asyncio
import contextlib
import logging
import os
from collections import deque
from dataclasses import asdict
from typing import Deque, Dict, List, Optional

from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import RedirectResponse
from pydantic import BaseModel, Field

from .common import PROTOCOL_NAMES, TelemetryRecord
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

telemetry_queue: "asyncio.Queue[TelemetryRecord]" = asyncio.Queue()
telemetry_source = create_source_from_env()
command_sender = telemetry_source.command_sender()
model = LinkQualityModel()

RECENT_HISTORY_LEN = 500
recent_records: Deque[TelemetryRecord] = deque(maxlen=RECENT_HISTORY_LEN)
latest_record_by_node: Dict[str, TelemetryRecord] = {}
latest_decision_by_node: Dict[str, Decision] = {}
ws_clients: List[WebSocket] = []
ingest_task: Optional[asyncio.Task] = None


class ForceProtocolRequest(BaseModel):
    node: str = Field(pattern="^(a|b)$")
    protocol: int = Field(ge=0, le=2)


@app.on_event("startup")
async def on_startup() -> None:
    global ingest_task
    ingest_task = asyncio.create_task(_ingest_loop())


@app.on_event("shutdown")
async def on_shutdown() -> None:
    if ingest_task is not None:
        ingest_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await ingest_task


@app.get("/", include_in_schema=False)
def read_root():
    """Redirect the base URL directly to the API documentation."""
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
    return {"nodes": data, "ingest": telemetry_source.counters()}


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
    """Raw passthrough (folded in from UART_Demo): sends an arbitrary line
    straight to Node A on D20/D21, bypassing the force_protocol JSON envelope.
    For probing the physical link directly, e.g. during bring-up."""
    if os.environ.get("SWAP_INPUT_MODE", "sim").strip().lower() != "serial":
        raise HTTPException(status_code=409, detail="Only available in SWAP_INPUT_MODE=serial")
    # Local import: arduino.app_utils only exists inside App Lab's own Python
    # environment on the board. Deferred so this module still imports cleanly
    # off the board (e.g. running the simulator locally) — this endpoint
    # already 409s before reaching this line unless SWAP_INPUT_MODE=serial.
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

    if decision.protocol != status.active_protocol:
        await command_sender.force_protocol(node=node, protocol=decision.protocol)
    latest_decision_by_node[node] = decision

    return {
        "node": node,
        "current_protocol": status.active_protocol,
        "recommended_protocol": decision.protocol,
        "recommended_protocol_name": PROTOCOL_NAMES[decision.protocol],
        "confidence": decision.confidence,
        "decision_source": decision.source,
        "forced": decision.protocol != status.active_protocol,
        "raw_scores": decision.raw_scores,
        "probabilities": decision.probabilities,
        "lqi": decision.lqi,
        "hysteresis_guarded": decision.hysteresis_guarded,
        "dwell_guarded": decision.dwell_guarded,
    }


@app.post("/force")
async def force_protocol(request: ForceProtocolRequest) -> Dict[str, object]:
    await command_sender.force_protocol(node=request.node, protocol=request.protocol)
    return {
        "ok": True,
        "node": request.node,
        "protocol": request.protocol,
        "protocol_name": PROTOCOL_NAMES[request.protocol],
    }


@app.websocket("/ws/live")
async def live_stream(websocket: WebSocket) -> None:
    await websocket.accept()
    ws_clients.append(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        if websocket in ws_clients:
            ws_clients.remove(websocket)


async def _ingest_loop() -> None:
    source_task = asyncio.create_task(telemetry_source.run(telemetry_queue))
    try:
        while True:
            record = await telemetry_queue.get()
            recent_records.append(record)
            latest_record_by_node[record.node] = record

            decision = model.observe(record)
            if decision is not None:
                latest_decision_by_node[record.node] = decision

            await _broadcast_ws(record=record, decision=decision)
    finally:
        source_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await source_task


async def _broadcast_ws(record: TelemetryRecord, decision: Optional[Decision]) -> None:
    if not ws_clients:
        return
    payload = {
        "record": asdict(record),
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
            "best_candidate": decision.best_candidate,
        },
    }
    stale_clients: List[WebSocket] = []
    for client in ws_clients:
        try:
            await client.send_json(payload)
        except Exception:
            stale_clients.append(client)
    for client in stale_clients:
        if client in ws_clients:
            ws_clients.remove(client)


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
        **extra,
    }
