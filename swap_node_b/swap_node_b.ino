/*
============================================================
              SWAP NODE B - COMMAND-DRIVEN FINAL
============================================================

ESP32 DevKit V1
Wi-Fi + BLE + SX1262 LoRa

Mirror of SWAP Node A with roles flipped:
  - WiFi: STATION connecting to Node A's "SWAP-A" AP.
  - BLE:  CENTRAL/CLIENT connecting to Node A's peripheral.
  - LoRa: symmetric.

Node B NEVER decides or searches for another protocol on its own.
It boots on LoRa (matches Node A -- the one link that doesn't need a
peer already listening on a specific SSID/GATT service) and retries
LoRa indefinitely until it comes up. It changes protocol ONLY after
receiving a "C:<N>" command from Node A. There is no automatic
fallback or round-robin switching. Node A is the master.

Node A now negotiates before switching: it sends "I:<N>" first and
waits for Node B to ack with "K:<N>" before committing. Node B's part
in that is passive -- reply to "I:<N>", change nothing else, and
switch only when the real "C:<N>" arrives, exactly as before.

============================================================
DATA FLOW
============================================================

Node B -> Node A, every PERCEPTION_INTERVAL_MS:
    "D:<rssi>"            WiFi / BLE
    "D:<rssi>,<snr>"       LoRa

Node A -> Node B (negotiation ping, no state change on B):
    "I:<N>"  ->  Node B replies "K:<N>" immediately

Node A -> Node B (received, triggers an immediate local switch):
    "C:<N>"
============================================================
WIFI (connects to Node A's AP)
============================================================

SSID:     SWAP-A
PASSWORD: SWAP12345
NODE A:   192.168.4.1 : 4210

============================================================
BLE (connects to Node A's peripheral)
============================================================

NAME (this board): SWAP-B
SERVICE (Node A's): 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
Node A's RX (we write here):     6E400002-B5A3-F393-E0A9-E50E24DCCA9E
Node A's TX (we subscribe here): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E

============================================================
SX1262 (identical wiring/params to Node A)
============================================================

NSS       GPIO13
DIO1      GPIO26
RESET     GPIO33
BUSY      GPIO27
RXEN      GPIO25
TXEN      GPIO32

SPI: MOSI GPIO23  MISO GPIO19  SCK GPIO18

866 MHz, 125 kHz, SF7, CR4/5, Sync 0x12, 14 dBm, Preamble 8, TCXO 1.6V
============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <RadioLib.h>

// ============================================================
// TIMING
// ============================================================

#define RECONNECT_RETRY_MS     3000UL
// How long an in-flight association is given before a retry may re-arm it.
// Longer than RECONNECT_RETRY_MS on purpose: a 3 s retry used to land in the
// middle of a still-running connect and get refused by ESP-IDF.
#define WIFI_ASSOC_SETTLE_MS   6000UL
#define PERCEPTION_INTERVAL_MS 4000UL
#define SWITCH_FLUSH_DELAY_MS  150UL

// Must match Node A's trial windows exactly. Node B has no UART back to
// UNO Q, so it can't be told to roll back -- it runs this same timeout
// independently, starting within SWITCH_FLUSH_DELAY_MS of Node A's own
// switch, so both boards fall back to previousProtocol together without
// needing to talk over a link that (if the trial failed) doesn't exist.
#define WIFI_TRIAL_MS 5000UL
#define BLE_TRIAL_MS  5000UL
#define BLE_SWITCH_GUARD_MS 3000UL   // wait 3 s after BLE connects before accepting a switch command
                                // pre-scan delay was trimmed so this window
                                // is actually reachable now.
#define LORA_TRIAL_MS 5000UL   // connectionless, radio-ready is the bar


// ============================================================
// WIFI (station -> Node A's AP)
// ============================================================

const char WIFI_SSID[] = "SWAP-A";
const char WIFI_PASS[] = "SWAP12345";

#define WIFI_PORT 4210

IPAddress nodeAIP(192, 168, 4, 1);

WiFiClient wifiClient;

bool wifiConnected = false;


// ============================================================
// BLE (central -> Node A's peripheral)
// ============================================================

const char BLE_NAME[] = "SWAP-B";

#define BLE_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

constexpr uint32_t BLE_SCAN_MS = 500UL;

NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* bleRemoteRx = nullptr;  // Node A's RX -- we write here
NimBLERemoteCharacteristic* bleRemoteTx = nullptr;  // Node A's TX -- we subscribe here

volatile bool bleConnected = false;
volatile uint32_t bleConnectedSinceMs = 0;
bool bleInitialized = false;


// ============================================================
// LORA (identical to Node A)
// ============================================================

#define LORA_NSS     13
#define LORA_DIO1    26
#define LORA_RESET   33
#define LORA_BUSY    27

#define LORA_RXEN    25
#define LORA_TXEN    32

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RESET, LORA_BUSY);

static const uint32_t rfPins[] = {
  LORA_RXEN, LORA_TXEN, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
};

static const Module::RfSwitchMode_t rfTable[] = {
  { Module::MODE_IDLE, { LOW,  LOW,  LOW, LOW, LOW } },
  { Module::MODE_RX,   { HIGH, LOW,  LOW, LOW, LOW } },
  { Module::MODE_TX,   { LOW,  HIGH, LOW, LOW, LOW } },
  END_OF_MODE_TABLE
};

bool loraReady = false;
volatile bool loraReceived = false;
float loraOwnRssi = -999.0f;
float loraOwnSnr = -999.0f;
bool loraOwnCrcOk = false;
unsigned long loraOwnToaMs = 0;

#define LORA_TX_POWER_DBM 14
#define LORA_SF 7
#define LORA_BW_KHZ 125
#define LORA_CR 5


// ============================================================
// PROTOCOL STATE
// ============================================================

enum Protocol { WIFI_PROTOCOL = 0, BLE_PROTOCOL = 1, LORA_PROTOCOL = 2 };

// Node B only ever has STABLE/TRIALING -- it never negotiates (that's
// Node A pinging "I:<N>" and waiting for our "K:<N>" reply, which we
// answer without changing state; see handleIncomingFromA()). We only
// enter TRIALING when the real "C:<N>" arrives. No switch_result either
// -- Node B has no UART link to UNO Q, only Node A reports outcomes.
enum SwitchState { STATE_STABLE = 0, STATE_TRIALING = 1, STATE_RECOVERING = 2 };

// Boots on LoRa, matching Node A -- see the header note.
Protocol activeProtocol = LORA_PROTOCOL;
Protocol previousProtocol = LORA_PROTOCOL;
uint32_t lastRetryAttempt = 0;

SwitchState switchState = STATE_STABLE;
uint32_t trialStartMs = 0;
uint32_t recoveryStartMs = 0;
bool linkWasEverConnected = false;

// Startup handshake: Node B has no direct UART link to UNO Q, so it announces
// initialization over boot LoRa and waits for Node A to relay the event and
// acknowledge it. Repetition makes the handshake robust to packet loss.
bool bootAcked = false;
uint32_t lastBootAnnouncementMs = 0;
#define BOOT_ANNOUNCE_INTERVAL_MS 1000UL

// Commands received through BLE callbacks are deferred until loop(). A BLE
// callback must not tear down or reinitialize the NimBLE host while the host
// is still executing that callback.
volatile int pendingSwitchTarget = -1;
volatile bool pendingSwitchCommand = false;

// LoRa trial proof: connectionless, so a recent packet from Node A is the
// only meaningful peer-connection signal.
uint32_t lastPeerPacketMs = 0;
bool havePeerPacket = false;

// Explicit declarations: do not depend on Arduino IDE prototype generation.
void switchProtocol(Protocol target);
size_t sendRawToA(const String& msg);
void handleIncomingFromA(const String& msg);

void switchProtocol(Protocol target);
size_t sendRawToA(const String& msg);
void handleIncomingFromA(const String& msg);



// ============================================================
// INCOMING MESSAGE HANDLER (from Node A)
// ============================================================

void handleIncomingFromA(const String& msg) {
  // Any valid incoming message from Node A proves peer traffic on LoRa.
  if (activeProtocol == LORA_PROTOCOL) {
    havePeerPacket = true;
    lastPeerPacketMs = millis();
  }

  // Probe (spec section 28): echo the sequence number straight back so
  // Node A can close its RTT measurement. Answered immediately.
  if (msg.startsWith("P:")) {
    sendRawToA("R:" + msg.substring(2));
    return;
  }

  // Boot acknowledgement from Node A. Once this reaches Node B, the
  // startup announcement has successfully reached the UNO Q through Node A.
  if (msg == "BOOT_ACK:B") {
    bootAcked = true;
    Serial.println("[BOOT] Node B initialization acknowledged by Node A");
    return;
  }

  // Negotiation ping: reply immediately, but don't touch our own state --
  // this is Node A checking we're reachable before it commits to
  // anything, not a command to switch. We only actually switch on "C:".
  // Only ack while STABLE: if we're somehow mid-switch ourselves (should
  // not happen in lockstep with Node A's own lockout, but defensively),
  // staying silent here makes Node A's negotiation time out and report a
  // rejection -- a safe failure mode instead of a compounded desync.
  if (msg.startsWith("I:")) {
    if (switchState == STATE_STABLE) {
      sendRawToA("K:" + msg.substring(2));
    }
    return;
  }

  if (!msg.startsWith("C:")) {
    return;  // Node B has nothing else to receive from Node A right now.
  }

  int target = msg.substring(2).toInt();
  if (target < 0 || target > 2) {
    return;
  }

  Serial.print("[SWITCH] Node A says -> ");
  Serial.println(target);

  // Never execute a protocol teardown from inside the BLE notification
  // callback. Queue it and let loop() perform the actual switch.
  pendingSwitchTarget = target;
  pendingSwitchCommand = true;
}


// ============================================================
// BLE CLIENT CALLBACK
// ============================================================

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* client) override {
    bleConnected = true;
    bleConnectedSinceMs = millis();
    Serial.println("[BLE] CONNECTED");
  }

  void onDisconnect(NimBLEClient* client, int reason) override {
    bleConnected = false;
    bleConnectedSinceMs = 0;
    Serial.println("[BLE] DISCONNECTED");
  }
};

ClientCallbacks clientCallbacks;

void onBleNotify(NimBLERemoteCharacteristic* remoteChar, uint8_t* data, size_t len, bool isNotify) {
  String message;
  for (size_t i = 0; i < len; i++) {
    message += (char)data[i];
  }
  handleIncomingFromA(message);
}


// ============================================================
// LORA INTERRUPT
// ============================================================

void IRAM_ATTR loraFlag() {
  loraReceived = true;
}


// ============================================================
// WIFI START / STOP / SERVICE
// ============================================================

void startWiFi() {
  Serial.println("[WIFI] START (connecting to SWAP-A)");

  wifiConnected = false;

  WiFi.mode(WIFI_MODE_NULL);
  delay(30);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  lastRetryAttempt = millis();
}

void stopWiFi() {
  Serial.println("[WIFI] OFF");

  wifiClient.stop();
  wifiConnected = false;

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_MODE_NULL);

  delay(30);
}

// True only when the supplicant has settled -- i.e. the previous association
// attempt has finished, one way or the other. WiFi.begin() calls
// esp_wifi_set_config(), which ESP-IDF refuses while a connect is still in
// flight: that is the "E (nnnnn) wifi:sta is connecting, cannot set config"
// error. The refused begin() is dropped on the floor, so a retry loop that
// fires blind can keep re-arming a connection that never actually restarts.
bool wifiSupplicantSettled() {
  switch (WiFi.status()) {
    case WL_CONNECT_FAILED:
    case WL_NO_SSID_AVAIL:
    case WL_CONNECTION_LOST:
      return true;   // terminal outcome, safe to re-arm immediately
    case WL_IDLE_STATUS:
    case WL_DISCONNECTED:
      // Ambiguous on ESP32: both the "not started" and the "still associating"
      // state report these. Treat as settled only once the attempt has had
      // longer than an association normally takes.
      return millis() - lastRetryAttempt >= WIFI_ASSOC_SETTLE_MS;
    default:
      return false;  // WL_CONNECTED / WL_SCAN_COMPLETED: nothing to re-arm
  }
}

void serviceWiFi() {
  if (!wifiConnected) {
    if (WiFi.status() == WL_CONNECTED && !wifiClient.connected()) {
      if (wifiClient.connect(nodeAIP, WIFI_PORT)) {
        wifiClient.setNoDelay(true);
        wifiConnected = true;
        Serial.println("[WIFI] CONNECTED TO NODE A");
      }
    } else if (WiFi.status() != WL_CONNECTED &&
               millis() - lastRetryAttempt >= RECONNECT_RETRY_MS &&
               wifiSupplicantSettled()) {
      // Retry forever, throttled -- no give-up/auto-switch.
      lastRetryAttempt = millis();
      // Explicitly leave the previous attempt before re-arming, so
      // esp_wifi_set_config() is never called on a connecting STA.
      WiFi.disconnect(false, false);
      delay(20);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }

  while (wifiClient.available()) {
    String line = wifiClient.readStringUntil('\n');
    line.trim();
    if (line.length()) {
      handleIncomingFromA(line);
    }
  }

  if (!wifiClient.connected() || WiFi.status() != WL_CONNECTED) {
    wifiClient.stop();
    wifiConnected = false;
    Serial.println("[WIFI] Lost Node A -- retrying");
  }
}


// ============================================================
// BLE START / STOP / SERVICE
// ============================================================

void startBLE() {
  Serial.println("[BLE] START (scanning for SWAP-A)");

  bleConnected = false;
  bleConnectedSinceMs = 0;

  // Initialize NimBLE only once. Protocol switching stops the client and
  // scanner but keeps the BLE host/controller alive for later trials.
  if (!bleInitialized) {
    if (!NimBLEDevice::init(BLE_NAME)) {
      Serial.println("[BLE] INIT FAILED");
      return;
    }

    NimBLEDevice::setPower(9);
    bleInitialized = true;
  }

  bleRemoteRx = nullptr;
  bleRemoteTx = nullptr;

  // Start immediately; do not burn the first half of the 5-second trial on
  // a retry delay.
  lastRetryAttempt = millis() - RECONNECT_RETRY_MS;
}

void stopBLE() {
  Serial.println("[BLE] OFF");

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan != nullptr && scan->isScanning()) {
    scan->stop();
  }

  if (bleClient != nullptr) {
    // deleteClient() itself disconnects/stops the client if needed.
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
  }

  bleConnected = false;
  bleRemoteRx = nullptr;
  bleRemoteTx = nullptr;

  // Intentionally no NimBLEDevice::deinit(). Reinitializing the whole BLE
  // host/controller on every protocol transition is not necessary and makes
  // repeated BLE bring-up much less reliable.
  delay(50);
}

void attemptBleConnect() {
  Serial.println("[BLE] SCANNING...");

  NimBLEScan* scan = NimBLEDevice::getScan();
  NimBLEScanResults results = scan->getResults(BLE_SCAN_MS, false);

  const NimBLEAdvertisedDevice* target = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* device = results.getDevice(i);
    if (device->isAdvertisingService(NimBLEUUID(BLE_SERVICE))) {
      target = device;
      break;
    }
  }

  if (target == nullptr) {
    Serial.println("[BLE] Node A not found this scan");
    return;
  }

  if (bleClient != nullptr) {
    if (bleClient->isConnected()) bleClient->disconnect();
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
  }

  bleClient = NimBLEDevice::createClient();
  if (bleClient == nullptr) {
    Serial.println("[BLE] CLIENT CREATE FAILED");
    return;
  }

  bleClient->setClientCallbacks(&clientCallbacks, false);
  bleClient->setConnectTimeout(2000);
  bleClient->setConnectRetries(0);

  if (!bleClient->connect(target)) {
    Serial.println("[BLE] CONNECT FAILED");
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
    return;
  }

  NimBLERemoteService* svc = bleClient->getService(BLE_SERVICE);
  if (svc == nullptr) {
    Serial.println("[BLE] SERVICE NOT FOUND");
    bleClient->disconnect();
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
    return;
  }

  bleRemoteRx = svc->getCharacteristic(BLE_RX);
  bleRemoteTx = svc->getCharacteristic(BLE_TX);

  if (bleRemoteRx == nullptr || !bleRemoteRx->canWrite()) {
    Serial.println("[BLE] RX CHARACTERISTIC INVALID");
    bleClient->disconnect();
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
    bleRemoteRx = nullptr;
    bleRemoteTx = nullptr;
    return;
  }

  if (bleRemoteTx == nullptr || !bleRemoteTx->canNotify()) {
    Serial.println("[BLE] TX CHARACTERISTIC INVALID");
    bleClient->disconnect();
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
    bleRemoteRx = nullptr;
    bleRemoteTx = nullptr;
    return;
  }

  if (!bleRemoteTx->subscribe(true, onBleNotify)) {
    Serial.println("[BLE] NOTIFICATION SUBSCRIBE FAILED");
    bleClient->disconnect();
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
    bleRemoteRx = nullptr;
    bleRemoteTx = nullptr;
    return;
  }

  bleConnected = true;
  bleConnectedSinceMs = millis();
  Serial.print("[BLE] LINK READY, RSSI=");
  Serial.println(bleClient->getRssi());
}


void serviceBLE() {
  if (!bleConnected) {
    if (millis() - lastRetryAttempt >= RECONNECT_RETRY_MS) {
      lastRetryAttempt = millis();
      attemptBleConnect();
    }
    return;
  }
  // Connected: incoming data arrives via onBleNotify callback.
}


// ============================================================
// LORA START / STOP / SERVICE
// ============================================================

void startLoRa() {
  Serial.println("[LORA] START");

  loraReady = false;
  loraReceived = false;
  loraOwnRssi = -999.0f;
  loraOwnSnr = -999.0f;
  loraOwnCrcOk = false;
  loraOwnToaMs = 0;

  int16_t result = radio.begin(866.0, 125.0, 7, 5, 0x12, 14, 8, 1.6);

  if (result != RADIOLIB_ERR_NONE) {
    Serial.print("[LORA] INIT ERROR: ");
    Serial.println(result);
    return;
  }

  radio.setRfSwitchTable(rfPins, rfTable);
  radio.setCurrentLimit(80.0);
  radio.setCRC(2);
  radio.explicitHeader();
  radio.setPacketReceivedAction(loraFlag);
  radio.startReceive();

  loraReady = true;
  Serial.println("[LORA] READY 866 MHz");
}

void stopLoRa() {
  Serial.println("[LORA] OFF");

  if (loraReady) {
    radio.sleep();
  }

  loraReady = false;
  loraReceived = false;
}

void serviceLoRa() {
  if (!loraReady) {
    if (millis() - lastRetryAttempt >= RECONNECT_RETRY_MS) {
      lastRetryAttempt = millis();
      startLoRa();
    }
    return;
  }

  if (!loraReceived) {
    return;
  }

  loraReceived = false;

  String data;
  int16_t result = radio.readData(data);

  loraOwnCrcOk = (result == RADIOLIB_ERR_NONE);

  if (result == RADIOLIB_ERR_NONE) {
    loraOwnRssi = radio.getRSSI();
    loraOwnSnr = radio.getSNR();
    // getTimeOnAir() returns MICROseconds (confirmed in SX126x.h) -- convert
    // to ms so the reported field matches its name.
    loraOwnToaMs = radio.getTimeOnAir(data.length()) / 1000UL;

    if (data.length() > 0) {
      handleIncomingFromA(data);
    }
  }
}


// ============================================================
// PERCEPTION DATA -> NODE A
// ============================================================

// Single outbound path to Node A over whichever protocol is active.
// Returns bytes sent, or 0 if the link wasn't up.
size_t sendRawToA(const String& msg) {
  switch (activeProtocol) {
    case WIFI_PROTOCOL:
      if (!wifiConnected) return 0;
      wifiClient.print(msg);
      wifiClient.print('\n');
      return msg.length();

    case BLE_PROTOCOL:
      if (!bleConnected || bleRemoteRx == nullptr || !bleRemoteRx->canWrite()) return 0;
      bleRemoteRx->writeValue(msg.c_str(), false);
      return msg.length();

    case LORA_PROTOCOL: {
      if (!loraReady) return 0;
      String tx = msg;          // transmit() takes a non-const String&
      radio.transmit(tx);       // blocking
      radio.startReceive();     // must re-arm RX after every TX
      return msg.length();
    }
  }
  return 0;
}

void sendPerceptionData() {
  String msg;
  // connected/ready is implicit in the protocol-specific source below;
  // the master uses its own telemetry plus probe acknowledgements for
  // actual link-quality decisions.

  switch (activeProtocol) {
    case WIFI_PROTOCOL:
      if (!wifiConnected) return;
      // "D:<rssi>,<channel>"
      // WiFi.RSSI() returns int8_t -- String(intValue, 1) resolves to the
      // (value, base) overload, not (value, decimalPlaces), and base 1 is
      // invalid, silently producing "0". Casting to float forces the
      // correct decimal-places overload. This was the actual cause of the
      // rssi=0.00 readings.
      msg = "D:" + String((float)WiFi.RSSI(), 1) + "," + String(WiFi.channel());
      break;

    case BLE_PROTOCOL:
      if (!bleConnected) return;
      // "D:<rssi>" -- nothing else cheaply available from the client API.
      msg = "D:" + String(bleClient->getRssi());
      break;

    case LORA_PROTOCOL:
      if (!loraReady) return;
      // "D:<rssi>,<snr>,<crc_ok 0|1>,<time_on_air_ms>"
      msg = "D:" + String(loraOwnRssi, 1) + "," + String(loraOwnSnr, 1) + ","
          + String(loraOwnCrcOk ? 1 : 0) + "," + String(loraOwnToaMs);
      break;
  }

  sendRawToA(msg);
}


// ============================================================
// PROTOCOL SWITCH (triggered only by a "C:" command from Node A)
// ============================================================

void stopActiveProtocol() {
  switch (activeProtocol) {
    case WIFI_PROTOCOL: stopWiFi(); break;
    case BLE_PROTOCOL:  stopBLE();  break;
    case LORA_PROTOCOL: stopLoRa(); break;
  }
}

void startActiveProtocol() {
  switch (activeProtocol) {
    case WIFI_PROTOCOL: startWiFi(); break;
    case BLE_PROTOCOL:  startBLE();  break;
    case LORA_PROTOCOL: startLoRa(); break;
  }
}


uint32_t trialWindowMs(Protocol p) {
  switch (p) {
    case WIFI_PROTOCOL: return WIFI_TRIAL_MS;
    case BLE_PROTOCOL:  return BLE_TRIAL_MS;
    case LORA_PROTOCOL: return LORA_TRIAL_MS;
  }
  return WIFI_TRIAL_MS;
}

// Same bar as Node A's trialSucceeded(): WiFi/BLE need an actual peer
// connection, LoRa just needs the radio up (connectionless).
bool trialSucceeded() {
  switch (activeProtocol) {
    case WIFI_PROTOCOL: return wifiConnected;
    case BLE_PROTOCOL:  return bleConnected;
    case LORA_PROTOCOL:
      return loraReady && havePeerPacket && (millis() - lastPeerPacketMs <= LORA_TRIAL_MS);
  }
  return false;
}

bool activeProtocolConnected() {
  switch (activeProtocol) {
    case WIFI_PROTOCOL: return wifiConnected;
    case BLE_PROTOCOL:  return bleConnected;
    case LORA_PROTOCOL:
      return loraReady && havePeerPacket && (millis() - lastPeerPacketMs <= LORA_TRIAL_MS);
  }
  return false;
}

void beginLinkRecovery() {
  if (switchState != STATE_STABLE) return;
  if (activeProtocolConnected()) return;

  recoveryStartMs = millis();
  switchState = STATE_RECOVERING;
  Serial.print("[RECOVERY] ");
  Serial.print((int)activeProtocol);
  Serial.println(" link lost -- 5-second recovery window started");
}

void serviceLinkRecovery() {
  if (switchState != STATE_RECOVERING) return;

  if (activeProtocolConnected()) {
    switchState = STATE_STABLE;
    Serial.println("[RECOVERY] Link recovered");
    return;
  }

  if (millis() - recoveryStartMs < 5000UL) return;

  Protocol failed = activeProtocol;
  Serial.print("[RECOVERY] protocol ");
  Serial.print((int)failed);
  Serial.print(" did not recover in 5 s -- rolling back to ");
  Serial.println((int)previousProtocol);

  stopActiveProtocol();
  activeProtocol = previousProtocol;
  startActiveProtocol();
  linkWasEverConnected = false;
  switchState = STATE_STABLE;
}

// Node A now locks out overlapping force_protocol commands, so a
// duplicate/stray "C:" shouldn't normally arrive mid-trial. Guarded here
// too anyway: same reasoning as Node A -- previousProtocol is only a
// trustworthy rollback target if it was actually STABLE, and Node B has
// no UART to complain to UNO Q, so it just silently defers.
void switchProtocol(Protocol target) {
  if (target == activeProtocol) {
    return;
  }
  if (switchState == STATE_TRIALING) {
    Serial.print("[SWITCH] Ignored -- trial of protocol ");
    Serial.print((int)activeProtocol);
    Serial.println(" still in progress");
    return;
  }

  // Give Node A's "C:" command a moment to have actually landed (we're
  // reacting to it right now, so this is mostly about our own outbound
  // traffic settling) before tearing this protocol down.
  delay(SWITCH_FLUSH_DELAY_MS);

  previousProtocol = activeProtocol;
  stopActiveProtocol();
  activeProtocol = target;

  // A LoRa trial must see fresh traffic after the switch; never inherit
  // stale packets from the previous LoRa session.
  if (activeProtocol == LORA_PROTOCOL) {
    havePeerPacket = false;
    lastPeerPacketMs = 0;
  }

  startActiveProtocol();

  switchState = STATE_TRIALING;
  trialStartMs = millis();

  Serial.print("[SWITCH] Trialing protocol ");
  Serial.println((int)activeProtocol);
}

// Every loop(): if the trial window on the current (commanded) protocol
// has elapsed without it proving itself, fall back to previousProtocol.
// No message to Node A -- see the header comment on WIFI_TRIAL_MS etc.
// Node A detects the same failure on its own side independently, since
// its "connected" state is literally whether Node B connected to it.
void checkTrialResult() {
  if (switchState != STATE_TRIALING) {
    return;
  }
  if (millis() - trialStartMs < trialWindowMs(activeProtocol)) {
    return;
  }

  if (trialSucceeded()) {
    linkWasEverConnected = true;
    switchState = STATE_STABLE;
    Serial.print("[TRIAL] SUCCESS on protocol ");
    Serial.println((int)activeProtocol);
    return;
  }

  Serial.print("[TRIAL] FAILED on protocol ");
  Serial.print((int)activeProtocol);
  Serial.print(" -- rolling back to ");
  Serial.println((int)previousProtocol);

  stopActiveProtocol();
  activeProtocol = previousProtocol;
  startActiveProtocol();
  switchState = STATE_STABLE;
}


// ============================================================
// DEFERRED COMMAND SERVICE
// ============================================================

void serviceBootAnnouncement() {
  // The initial protocol is LoRa. Do not send the startup handshake after a
  // later protocol switch. Repeat until Node A confirms receipt.
  if (bootAcked || activeProtocol != LORA_PROTOCOL || !loraReady) {
    return;
  }

  if (millis() - lastBootAnnouncementMs >= BOOT_ANNOUNCE_INTERVAL_MS) {
    lastBootAnnouncementMs = millis();
    if (sendRawToA("BOOT:B:LORA") > 0) {
      Serial.println("[BOOT] Node B initialized on LoRa -> Node A");
    }
  }
}


void servicePendingSwitchCommand() {
  if (!pendingSwitchCommand) {
    return;
  }

  noInterrupts();
  int target = pendingSwitchTarget;
  pendingSwitchCommand = false;
  pendingSwitchTarget = -1;
  interrupts();

  if (target >= 0 && target <= 2) {
    if (switchState == STATE_STABLE && activeProtocol == BLE_PROTOCOL && bleConnected) {
      uint32_t bleAge = millis() - bleConnectedSinceMs;
      if (bleAge < BLE_SWITCH_GUARD_MS) {
        // Preserve the command instead of dropping it. It will be retried
        // automatically once the BLE link has had 3 seconds to settle.
        pendingSwitchTarget = target;
        pendingSwitchCommand = true;
        return;
      }
    }
    switchProtocol((Protocol)target);
  }
}


// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("================================");
  Serial.println("   SWAP NODE B - COMMAND DRIVEN");
  Serial.println("================================");

  startActiveProtocol();

  // First announcement is attempted immediately; serviceBootAnnouncement()
  // will retry every second until Node A acknowledges it.
  if (activeProtocol == LORA_PROTOCOL && loraReady) {
    if (sendRawToA("BOOT:B:LORA") > 0) {
      Serial.println("[BOOT] Node B initialized on LoRa -> Node A");
    }
  }
}

void loop() {
  serviceBootAnnouncement();
  servicePendingSwitchCommand();

  // Service the active protocol before evaluating the trial deadline so a
  // packet/connect event arriving at the 5-second boundary can prove it.
  switch (activeProtocol) {
    case WIFI_PROTOCOL: serviceWiFi(); break;
    case BLE_PROTOCOL:  serviceBLE();  break;
    case LORA_PROTOCOL: serviceLoRa(); break;
  }

  if (switchState == STATE_STABLE) {
    if (activeProtocolConnected()) {
      linkWasEverConnected = true;
    } else if (linkWasEverConnected) {
      beginLinkRecovery();
    }
  }
  serviceLinkRecovery();
  checkTrialResult();

  static uint32_t lastSend = 0;
  if (millis() - lastSend >= PERCEPTION_INTERVAL_MS) {
    lastSend = millis();
    sendPerceptionData();
  }


  delay(2);
}
