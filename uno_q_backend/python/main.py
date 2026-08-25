"""App Lab entry point for the SWAP FastAPI backend on the UNO Q.

Runs swap_backend/app.py under uvicorn with SWAP_INPUT_MODE=serial, so
telemetry arrives via BridgeTelemetrySource -- the sketch on the MCU side
reads Node A's UART and relays each JSON line over the Router Bridge, and
force_protocol commands travel back down the same path.

Why this exists rather than uno_q_app: uno_q_app runs a simpler per-frame
model and only prints results, so nothing serves the dashboard and there is
no rolling-window/hysteresis logic. This app runs the real backend, which
the frontend already targets (/status, /ws/live, /force).
"""

import os
import sys
from pathlib import Path

# swap_backend resolves its model as the relative path
# "models/link_quality_rf.joblib", so the process must run from the
# directory that contains models/. App Lab's working directory isn't
# guaranteed, so pin it explicitly.
APP_DIR = Path(__file__).resolve().parent
os.chdir(APP_DIR)
sys.path.insert(0, str(APP_DIR))

# Must be set before swap_backend.app is imported: create_source_from_env()
# reads it at module import time to choose Bridge vs simulator.
os.environ.setdefault("SWAP_INPUT_MODE", "serial")
os.environ.setdefault("SWAP_CSV_LOG", str(APP_DIR / "telemetry_log.csv"))

import uvicorn  # noqa: E402  (after chdir/env setup, deliberately)

from swap_backend.app import app  # noqa: E402


if __name__ == "__main__":
    print(f"[swap-backend] serving from {APP_DIR}", flush=True)
    print(f"[swap-backend] input mode = {os.environ['SWAP_INPUT_MODE']}", flush=True)
    # host=0.0.0.0 so the dashboard can reach it from another machine on the
    # network, not just from the board itself.
    # Overridable so a port clash (uvicorn: "address already in use") can be
    # stepped around without editing this file. app.yaml exposes 8000.
    port = int(os.environ.get("SWAP_PORT", "8000"))
    print(f"[swap-backend] listening on 0.0.0.0:{port}", flush=True)
    uvicorn.run(app, host="0.0.0.0", port=port, log_level="info")
