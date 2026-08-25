# SWAP — Function Reference

Brief explanation of the main functions across the SWAP codebase, grouped by module. Format: `funcName() → what it does`.

---

## 1. SWAP_HindranceTool (ESP32 jamming/interference test tool)

### SWAP_HindranceTool.ino (serial-only build)

```
ina219WriteReg()            → writes a 16-bit value to an INA219 power-sensor register over I2C
ina219ReadReg()              → reads a 16-bit value from an INA219 register over I2C
ina219Init()                  → configures an INA219 (32V/2A range) and sets its calibration for a 0.1Ω shunt
ina219ReadBusVoltage()        → reads and converts the INA219 bus-voltage register to volts
ina219ReadCurrent()           → reads and converts the INA219 current register to mA
wifiSetChannel()              → sets the ESP32 WiFi radio to a given channel
wifiSendRaw()                 → transmits a raw 802.11 frame via esp_wifi_80211_tx
wifiRandomizeMac()            → randomizes a frame's source/BSSID bytes and sequence field
runWiFiDeauth()                → sends a randomized-MAC 802.11 deauthentication broadcast frame
runWiFiBeaconSpam()            → sends a fake-AP beacon frame with a randomized SSID character
runWiFiProbeSpam()              → sends a randomized-MAC broadcast probe-request frame
runWiFiAuthSpam()                → sends a randomized-MAC open-system authentication frame
runBLEAdvSpam()                   → advertises BLE packets with random manufacturer data/UUID/name
runBLEConnFlood()                  → opens and immediately tears down a BLE connection to a random MAC
runBLEPairingSpam()                 → connects to a random BLE address and requests pairing
loraConfigureForHindrance()          → sets SX1262 frequency/SF/BW/power and RF-switch pins
runLoRaCollision()                    → sends a timed packet to collide with SWAP's expected LoRa slot
runLoRaPreambleSpam()                  → transmits with max preamble length to jam preamble detection
runLoRaDutyAbuse()                      → fires 3 rapid packets to exceed the 1% LoRa duty-cycle limit
runHindranceLoop()                       → dispatches to the active WiFi/BLE/LoRa attack based on mode/targets
printMenu()                               → prints the serial command menu and current status
handleSerialCommand()                      → parses a serial keypress into a mode/target change
printStatus()                               → prints uptime, mode, packet counters, and power readings
processUnoCommand()  (Node A only)           → parses a UART command from the UNO Q and replies
checkUnoSerial()  (Node A only)               → buffers UART bytes until newline, then dispatches
setup()                                        → initializes I2C/power sensor, radio, WiFi, BLE, UART
loop()                                          → reads commands, runs the hindrance loop, prints status
```

### SWAP_HindranceTool_WebServer.ino (WiFi dashboard build)

Same INA219 / WiFi / BLE / LoRa attack functions as above, plus a WebSocket/HTTP layer:

```
wsEvent()                 → WebSocket callback: sends initial status on connect, routes incoming messages
handleWsMessage()          → parses an incoming WS message as JSON or plain-text and dispatches it
handleCommand()              → interprets a mode/control command and updates hindrance state
sendStatusToClient/All()      → sends the current mode/packet/uptime JSON to one or all WS clients
sendPowerToClient/All()        → reads both power rails and sends the reading to one or all WS clients
sendTargetsToClient/All()       → sends the active-targets bitmask to one or all WS clients
broadcastLog()                    → broadcasts a log message to all WS clients
requestUnoStatus()  (Node A only)  → asks the UNO Q for a status update every 5s
handleRoot()                        → serves the dashboard's index.html from flash storage
handleNotFound()                     → serves a matching file from flash, else 404
setup()                                → same as above, plus LittleFS + WebSocket + HTTP routes
loop()                                  → same as above, plus periodic WS status/power broadcasts
```

### hello_serial / lora_spi_diag / pin_integrity (bring-up/diagnostic sketches)

```
setup() [hello_serial]        → prints a banner and free-heap info to confirm the upload/serial path works
loop()  [hello_serial]         → prints an incrementing tick counter once per second

waitBusyLow()   [lora_spi_diag] → polls the SX1262 BUSY pin until low or timeout
hardwareReset()                  → toggles the radio's RST pin and confirms BUSY responds
decodeStatus()                    → prints the radio's status byte in human-readable form
probeBus()                         → tests whether the SPI bus can talk to the radio under given pin/speed settings
setup()                             → tries multiple SPI pin/speed combinations and reports which one works
loop()                               → idles (diagnosis runs once in setup)

testPin()      [pin_integrity]  → drives/reads one GPIO pin four ways to detect a stuck or damaged line
setup()                           → checks all four SPI lines (MOSI/SCK/MISO/NSS) for wiring faults
loop()                             → idles (test runs once in setup)
```

---

## 2. LoRa bring-up & node firmware

### lora_bringup_a / lora_bringup_b (ping-pong link test)

```
setup()                    → starts serial and initializes the LoRa radio
loop() [node A]             → sends "PING <seq>" every 1s and waits for the matching "PONG" reply
loop() [node B]              → waits for a "PING", logs RSSI/SNR, and echoes back "PONG"

setupLoRa()                    → configures RF-switch pins and initializes the SX1262 radio
sendLoRaTelemetry()             → checks the duty-cycle budget and transmits a payload
receiveLoRaTelemetry()           → receives a packet with timeout and updates RSSI/SNR metrics
getLoraMetrics()                  → returns the current radio metrics struct
dutyCanTransmit()                  → checks whether the next send would exceed the duty-cycle budget
dutyRecordTransmission()            → logs a transmission's airtime for duty-cycle tracking
dutyAirtimeUsedMs()                  → sums airtime of all currently-tracked transmissions
purgeOldDutyRecords()                 → drops duty-cycle records older than the 1-hour window
estimateAirTimeMs()                    → estimates a payload's on-air time from SF/BW/CR
onLoraDio1()                             → interrupt handler that flags a packet has arrived
noteRadioSuccess() / noteRadioFailureAndMaybeReinit()  → tracks consecutive radio faults and force-reinitializes after 5
```

### swap_node (shared library) / swap_node_a / swap_node_b (deployable node sketches)

```
LinkManager::begin()               → starts UART and initializes LoRa, WiFi, and BLE
LinkManager::beginWifi()            → brings up SoftAP (node A) or connects as a station (node B)
LinkManager::beginBle()              → sets up BLE server (node A) or scans/connects as client (node B)
LinkManager::tryWifiExchange()        → exchanges telemetry with the peer over WiFi/TCP
LinkManager::tryBleExchange()          → exchanges telemetry with the peer over BLE
LinkManager::tryLoraExchange()          → exchanges telemetry with the peer over LoRa
LinkManager::evaluateAndSwitch()         → picks the best protocol (WiFi > BLE > LoRa) from rolling link-health stats
LinkManager::pollIncomingCommands()       → reads JSON commands over UART and applies protocol overrides
LinkManager::emitTelemetryFrame()          → sends this node's metrics as a JSON line over UART
LinkManager::buildOwnPeerPayload()          → encodes this node's metrics as CSV to send to the peer
LinkManager::parsePeerPayload()              → parses the peer's CSV telemetry into local fields
LinkManager::loop()                            → polls commands, runs the active protocol, re-evaluates switching, emits telemetry
RollingMetrics::push/avgRssi/avgRtt/packetLossRate()  → maintains a rolling window of RSSI/RTT/loss samples

--- swap_node_a.ino / swap_node_b.ino (fuller standalone versions) ---
sendProbe() / onProbeAck()          → sends a numbered probe packet and matches the peer's ack to compute RTT
computeLinkStats()                   → computes packet loss, RTT, jitter, throughput, and RSSI stability
handleIncomingFromB() / handleIncomingFromA()  → parses incoming probe/perception/switch-command messages from the peer
startWiFi/stopWiFi/serviceWiFi()      → manages the WiFi transport (SoftAP+TCP server on A, station client on B)
startBLE/stopBLE/serviceBLE()          → manages the BLE transport (NimBLE server on A, client scan/connect on B)
startLoRa/stopLoRa/serviceLoRa()        → manages the LoRa transport
switchProtocol()                          → tears down the old transport and brings up the new one
checkAutoSwitch()                          → auto-advances to the next protocol if the current one fails to connect within 8s
readOwnWifiRssi() / readOwnBleRssi()        → reads real signal-strength values from the WiFi/BLE stack
emitUartFrame() [node A]                     → sends the combined perception+telemetry JSON frame to the UNO Q
sendPerceptionData() [node B]                  → sends this node's own RSSI/SNR/CRC reading to node A
setup() / loop()                                → bring up the initial protocol; service it and check for auto-switch each cycle
```

---

## 3. swap_backend (Python — telemetry ingest, ML routing, REST/WebSocket API)

```
app.py
  get_status()             → GET /status — returns per-node telemetry status and recommended protocol
  get_recent()               → GET /telemetry/recent — returns the most recent buffered telemetry records
  get_model_info()             → GET /model/info — returns diagnostics about the loaded ML model
  decide()                       → POST /decide — computes and applies the recommended protocol for a node
  force_protocol()                 → POST /force — forces a node onto a specific protocol
  live_stream()                      → WebSocket /ws/live — streams live telemetry/decisions to clients
  _ingest_loop()                       → background loop pulling telemetry, updating model state, and broadcasting it

common.py
  TelemetryRecord.from_json() / from_pinout_json()  → parse raw JSON telemetry lines (two wire-format versions) into records
  parse_telemetry_line()        → picks the right parser for an incoming line
  CsvLogger.append()              → appends one telemetry record as a CSV row

link_quality_model.py
  compute_raw_scores()                → computes WiFi/BLE/LoRa heuristic quality scores from feature means
  _forest_predict_proba()              → replicates RandomForest.predict_proba() via a manual leaf-walk (no joblib overhead)
  RuleBasedFallback.decide()            → deterministic rule-based protocol pick used when no trained model is loaded
  SwitchGovernor.gate()                  → enforces a minimum dwell time before allowing another protocol switch
  LinkQualityModel.observe()              → feeds a new telemetry record into a node's rolling window and returns a decision
  LinkQualityModel._predict_from_window()  → full decision pipeline: scoring → model or rule-based pick → confidence/hysteresis gates

simulator.py
  SimulatorTelemetrySource.run()          → runs both simulated nodes' telemetry generators concurrently
  SimulatorTelemetrySource._node_loop()     → generates, logs, and enqueues synthetic telemetry for one simulated node
  SimulatorTelemetrySource._phase()          → computes a sinusoidal signal-quality baseline for realism

telemetry_link.py
  BridgeTelemetrySource._on_telemetry_line()  → parses a real hardware telemetry line and enqueues it
  BridgeTelemetrySource.run()                   → hardware ingest loop (Arduino Bridge over serial)
  create_source_from_env()                        → picks simulator vs. real-hardware telemetry source from an env var

train_model.py (backend + laptop_training version)
  build_training_windows()   → slides a window over the CSV and computes labeled feature vectors
  main()                        → loads CSV, trains a RandomForest, evaluates it, and saves the model bundle

laptop_training/generate_dummy_data.py
  generate_dummy_data()        → generates a synthetic 600-row WiFi/BLE/LoRa dataset for pipeline testing

uno_q_app/python/main.py
  on_telemetry_line()           → runs the trained model on each incoming telemetry line and sends back a force_protocol command
```

---

## 4. swap-frontend (React + TypeScript dashboard — "Signal Orbit")

```
App()                    → wraps the app in ErrorBoundary, ThemeProvider, and the router
Home()                     → main dashboard: live WebSocket telemetry, active-protocol display, decision reasoning,
                              RSSI/loss/latency status, protocol comparison, signal history, and manual override controls
  choose(name)                → manually forces a given protocol via the API and shows a toast
  rssiFor(id) / lossFor(id)     → look up measured RSSI/loss for a given protocol from the current record
SwapApiClient                    → REST client (getStatus, getModelInfo, decideProtocol, forceProtocol, etc.)
SwapWebSocketClient                → manages the live telemetry WebSocket with auto-reconnect
useTheme()                           → hook for reading/setting the light/dark theme (persisted to localStorage)
```

## 5. swap-hindrance-web (React + Node/Express — hindrance-tool control panel)

```
backend/src/index.js
  handleClientMessage()      → routes UI WebSocket messages (ping, getStatus, command/mode/targets)
  handleNodeConnection()       → registers a connecting ESP32 node and pushes its initial config
  handleNodeMessage()            → routes messages from a node (register/status/telemetry/power/log) to the UI
  broadcastToClients() / sendToNode() / broadcastToNodes()  → message fan-out helpers
  periodic setInterval task         → prunes stale nodes (>30s) and polls all nodes for status every 10s

frontend/src
  AppContent()                → dashboard shell wiring WebSocket state to Header/ControlPanel/TelemetryPanel/etc.
    handleModeSelect()            → sets and sends a new hindrance mode
    handleTargetToggle()           → flips one target (wifi/ble/lora) and sends the new mask
    handleStop() / handleReset()    → sends the stop / counter-reset command
  useWebSocket()                → manages the WS connection: reconnect with backoff, heartbeat, message queueing
  ControlPanel / TelemetryPanel / PowerPanel / LogPanel / NodesPanel / NodeDetailModal / SettingsModal
                                 → presentational panels rendering mode controls, live stats, power readings,
                                   the event log, node list, per-node detail, and app settings
  useSettings() / useToast()     → hooks providing persisted settings and toast-notification state
```
