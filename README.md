# SWAP Prototype

Fully functional SWAP software prototype for adaptive Wi-Fi/BLE/LoRa decisioning.

## What is implemented

- FastAPI backend (`swap_backend.app`) with:
  - `GET /health`
  - `GET /status`
  - `GET /telemetry/recent?limit=50`
  - `POST /decide?node=a|b`
  - `POST /force`
  - `WS /ws/live`
- Telemetry ingest modes:
  - `SWAP_INPUT_MODE=sim` (default): built-in Node A/Node B telemetry simulator.
  - `SWAP_INPUT_MODE=serial`: reads JSON telemetry lines from real serial ports.
- Adaptive logic:
  - rolling-window feature extraction
  - model inference if `models/link_quality_model.joblib` exists
  - deterministic rule-based fallback if model is missing
- CSV logging for every telemetry frame (`telemetry_log.csv` by default).

## Run locally

1. Install dependencies:

```powershell
python -m pip install -r requirements.txt
```

2. Start API in simulator mode:

```powershell
python -m uvicorn swap_backend.app:app --host 0.0.0.0 --port 8000
```

3. Check status:

```powershell
curl http://127.0.0.1:8000/status
```

## Serial mode (real Node A / Node B)

Set environment variables then run uvicorn:

```powershell
$env:SWAP_INPUT_MODE="serial"
$env:SWAP_NODE_A_PORT="COM7"
$env:SWAP_NODE_B_PORT="COM8"
$env:SWAP_TELEMETRY_BAUD="19200"
python -m uvicorn swap_backend.app:app --host 0.0.0.0 --port 8000
```

Expected telemetry frame format (newline-delimited JSON):

```json
{"node":"a","ts_ms":1722500000000,"active_protocol":0,"wifi_rssi":-68,"wifi_loss":0.04,"ble_rssi":-78,"lora_rssi":-96,"lora_snr":5.3,"lora_loss":0.02,"rtt_ms":12}
```

Force command sent back to node (newline JSON):

```json
{"cmd":"force_protocol","protocol":2}
```

## Train a model from collected telemetry

```powershell
python -m swap_backend.train_model --input telemetry_log.csv --training models/training.csv --model models/link_quality_model.joblib
```

