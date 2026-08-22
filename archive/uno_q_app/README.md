# SWAP Link Predictor — Arduino UNO Q App

Deploys `laptop_training/swap_random_forest.joblib` (the RandomForestClassifier
trained by `laptop_training/train_model.py`, explicitly built for "the UNO Q
inference engine") onto the UNO Q's Linux (MPU) side, and drives a
Wi-Fi/BLE/LoRa indicator via the MCU (sketch) side.

The UNO Q has two processors, so this is a two-part app:

- `python/main.py` — runs on the Linux side. Loads the joblib model and calls
  `model.predict()` once per second.
- `sketch/sketch.ino` — runs on the real-time MCU. Currently returns
  **placeholder** telemetry (see the `TODO` in `get_telemetry()`) and lights
  one of three LEDs based on the predicted protocol. Replace the placeholder
  values with real RSSI/SNR/PDR/RTT reads from your Wi-Fi, BLE, and LoRa
  hardware.

## Project layout

```
uno_q_app/
├── app.yaml                 # App Lab manifest
├── python/
│   ├── main.py               # Linux-side inference loop
│   ├── requirements.txt      # joblib, numpy, pandas, scikit-learn
│   └── model/
│       ├── swap_random_forest.joblib
│       └── feature_sequence.txt
└── sketch/
    ├── sketch.ino             # MCU-side telemetry + LED indicator
    └── sketch.yaml            # fqbn + library deps
```

## Upload via Arduino IDE (App Lab) — GUI

1. Connect the UNO Q to this PC over USB and power it on.
2. Open the **Arduino IDE**. It should detect the UNO Q's App Lab
   environment (App Lab appears as a panel/tab once the board is selected —
   if it doesn't show up, install/update the **UNO Q board core** first via
   **Tools > Board > Boards Manager**, search "UNO Q").
3. In App Lab, choose **Open existing app** (or **File > Open**) and point it
   at this folder: `uno_q_app/`.
4. Select the UNO Q board and its port under **Tools > Board / Port**.
5. Click **Run/Upload** (▶). App Lab will:
   - install `python/requirements.txt` on the Linux side,
   - compile and flash `sketch/sketch.ino` to the MCU,
   - start `python/main.py`.
6. Watch the **Monitor**/serial log pane — you should see:
   ```
   Loaded model from .../swap_random_forest.joblib (features: [...])
   Bridge ready
   predicted=WIFI confidence=0.9x raw=[-65.0, -70.0, -95.0, 6.0, 0.98, 25.0, 0.0]
   ```

If your IDE version doesn't show an App Lab panel, use the CLI path below
instead — same files, no GUI needed.

## Upload via CLI (fallback, if App Lab GUI isn't available)

The UNO Q's Linux side exposes itself over USB as a network device with SSH.
From a terminal:

```bash
# find the board's IP (check Arduino IDE's board info, or your router / `arp -a`)
scp -r uno_q_app arduino@<UNO_Q_IP_ADDRESS>:~/ArduinoApps/uno_q_app
ssh arduino@<UNO_Q_IP_ADDRESS>
arduino-app-cli app start ~/ArduinoApps/uno_q_app
arduino-app-cli app logs ~/ArduinoApps/uno_q_app
```

Stop it with `arduino-app-cli app stop ~/ArduinoApps/uno_q_app`.

## Before you go further

- **Replace the placeholder telemetry** in `sketch/sketch.ino`'s
  `get_telemetry()` with real reads from your radios — right now it always
  returns the same fixed values, so the model will keep predicting the same
  protocol.
- The feature order is locked to `python/model/feature_sequence.txt`
  (`wifi_rssi,ble_rssi,lora_rssi,lora_snr,pdr,rtt,retries`) — if you retrain
  the model with different features, update both the CSV columns in
  `laptop_training/train_model.py` and `get_telemetry()` together.
- This model (100 trees, `max_depth=8`, 133 KB) is a *different* model from
  `models/link_quality_model.joblib` used by the FastAPI backend
  (`swap_backend/`) — that one uses rolling-window means and a different
  feature set, and is not the one deployed here.
