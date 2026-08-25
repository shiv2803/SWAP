# uno_q_backend — SWAP FastAPI backend as an UNO Q App Lab app

Runs `swap_backend` **on the UNO Q**, fed by the Router Bridge. This is the
app the dashboard (`swap-frontend`) talks to.

## Why this exists (and how it differs from `uno_q_app`)

`uno_q_app` predates the current firmware. Two problems:

1. **It can no longer parse the telemetry.** It reads the flat schema
   (`wifi_rssi`, `wifi_loss`, …) directly off each JSON line. Node A now
   emits the `link_state` / `link_rssi` / `node_b_telemetry` schema, so every
   frame fails with `KeyError: 'wifi_rssi'` and is dropped.
2. **It serves nothing.** It predicts per-frame and prints to the log — no
   rolling window, no hysteresis/dwell governor, no HTTP or WebSocket. The
   dashboard has nothing to connect to.

`swap_backend` already handles the new schema (`common.py::from_pinout_json`),
applies the windowed model plus the switching governor, implements the UNO Q control state machine (`control.py`), and exposes
`/status`, `/control`, `/decide`, `/force`, `/ws/live` and `/events`. This app just packages it for
App Lab.

**Run one or the other, not both** — two consumers of the same Bridge
notification will fight over `force_protocol`.

## Layout

```
uno_q_backend/
├── app.yaml                 # App Lab manifest (exposes port 8000)
├── python/
│   ├── main.py              # entry point: chdir, SWAP_INPUT_MODE=serial, uvicorn
│   ├── requirements.txt
│   ├── models/
│   │   └── link_quality_rf.joblib
│   └── swap_backend/        # copy of ../swap_backend
└── sketch/
    ├── sketch.ino           # copy of ../uno_q_mcu_sketch/sketch/sketch.ino
    └── sketch.yaml
```

`python/swap_backend/` and `python/models/` are **copies**, because an App Lab
app is deployed as one self-contained folder. `../swap_backend/` remains the
source of truth — re-copy after changing it:

```bash
cp swap_backend/*.py uno_q_backend/python/swap_backend/
cp models/link_quality_rf.joblib uno_q_backend/python/models/
```

## Deploy

From this PC (replace the IP with the board's — `hostname -I` on the board):

```bash
scp -r uno_q_backend arduino@192.168.170.37:~/ArduinoApps/uno_q_backend
```

Then on the board over SSH:

```bash
arduino-app-cli app stop ~/ArduinoApps/uno_q_app
arduino-app-cli app start ~/ArduinoApps/uno_q_backend
arduino-app-cli app logs ~/ArduinoApps/uno_q_backend
```

Stopping `uno_q_app` first matters — see the "one or the other" note above.

Expected log:

```
[swap-backend] serving from /app/python
[swap-backend] input mode = serial
INFO: Uvicorn running on http://0.0.0.0:8000
INFO:swap.telemetry:Bridge telemetry source ready, listening for 'telemetry_line' notifications
```

## Verify

```bash
curl http://<UNO_Q_IP>:8000/health
curl http://<UNO_Q_IP>:8000/status
```

`ingest.frames_ok` in `/status` should climb once Node A is powered and
wired to D0/D1. If it stays at 0, the UART path is the thing to check, not
this app — confirm the MCU sketch is flashed and Node A is transmitting at
115200.

## Point the dashboard at it

`swap-frontend/src/pages/Home.tsx` currently hardcodes `localhost:8000`.
For a board-hosted backend, change:

```js
const BASE_URL = "http://<UNO_Q_IP>:8000";
const WS_URL   = "ws://<UNO_Q_IP>:8000/ws/live";
```

CORS is already open (`allow_origins=["*"]` in `app.py`), so no backend
change is needed.

## Known limitation

`main.py` never calls `App.run()` — uvicorn's own loop keeps the process
alive instead, and `BridgeTelemetrySource` registers its handler from inside
that loop. This runs correctly under `arduino-app-cli app start`, but it has
not been tested against App Lab GUI lifecycle features (pause/resume) that
may expect `App.run()`.
