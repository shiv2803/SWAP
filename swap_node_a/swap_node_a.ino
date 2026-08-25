/*
============================================================
              SWAP NODE A - COMMAND-DRIVEN FINAL
============================================================

ESP32 DevKit V1
Wi-Fi + BLE + SX1262 LoRa

Switching is CONDITIONAL, not automatic. There is no timer-based
round robin anymore -- Node A stays on one protocol indefinitely,
continuously exchanging data with Node B, until the UNO Q tells it
to move via UART. It does not decide to switch on its own.

Boots on LoRa, not Wi-Fi. LoRa is the one link that comes up without
needing a peer already listening on a specific SSID/GATT service, so
it's the safe first thing both boards establish. UNO Q decides (from
the telemetry stream) when to start commanding Wi-Fi/BLE trials --
that's a backend decision, not enforced here.

A commanded switch is now two-phase, not silent:
  1. NEGOTIATING -- Node A pings Node B ("I:<N>") over the CURRENT,
     still-good link and waits for an ack ("K:<N>"), retrying a few
     times. Nothing is torn down yet. If Node B never acks, the
     switch is abandoned and reported to UNO Q as rejected -- this is
     what stops an unsynchronized unilateral switch (Node A switching
     away while Node B silently isn't listening).
  2. TRIALING -- only once acked: the real "C:<N>" goes out, both
     boards tear down the old protocol and bring up the new one, and
     it has to prove itself within its trial window or both roll back.

While a switch is in flight (NEGOTIATING or TRIALING), Node A refuses
any further force_protocol commands until it resolves back to STABLE
(success, failed rollback, or rejected). This prevents previousProtocol
from ever getting overwritten with an unproven protocol mid-switch --
see switchProtocol() and emitSwitchRejected().

previousProtocol always holds the last protocol that was actually
STABLE -- either genuinely proven by a trial, or the LoRa floor at
boot. Rollback therefore always lands on the last *trusted* protocol,
not necessarily LoRa: e.g. LoRa -> Wi-Fi (succeeds) -> BLE (fails)
rolls back to Wi-Fi, not LoRa. LoRa only becomes the resting point when
nothing else has been proven yet.

============================================================
DATA FLOW
============================================================

Node B -> Node A (over whichever protocol is active):
    "D:<rssi>"           WiFi / BLE  (Node B's own reading)
    "D:<rssi>,<snr>"      LoRa        (Node B's own reading)

Node A -> UNO Q (UART, TELEMETRY_SERIAL, every PERCEPTION_INTERVAL_MS):
    {"link_state":"wifi|ble|lora","timestamp":<s>,
     "link_rssi":<Node A's own reading or -999 if unavailable>,
     "link_snr":<LoRa only>,
     "node_b_telemetry":{"rssi":<B's last report>,"snr":<LoRa only>}}
    Matches swap_backend/common.py's TelemetryRecord.from_pinout_json.

UNO Q -> Node A (UART command, triggers a negotiated switch):
    {"cmd":"force_protocol","node":"a","protocol":N}

Node A -> Node B (wireless, negotiation phase, over whichever
protocol is CURRENTLY active -- repeated on a timer until acked):
    "I:<N>"   "I'm about to switch us to protocol N, are you there?"

Node B -> Node A (wireless, reply to "I:<N>", no state change on B):
    "K:<N>"   "Yes, go ahead."

Node A -> Node B (wireless, sent only after a "K:<N>" ack, BEFORE
Node A itself switches):
    "C:<N>"
    All three are 2-byte tags, not JSON -- this rides the constrained
    wireless link (BLE MTU, LoRa airtime), so each costs O(1) bytes
    instead of a JSON envelope. Node B only actually switches to
    protocol N on receiving "C:<N>", so both boards land on the new
    protocol together instead of drifting apart on independent timers
    -- and because "C:" only ever follows a successful "I:"/"K:"
    round trip, Node A knows Node B is listening before either side
    commits to anything.

============================================================
WIFI
============================================================

SSID:     SWAP-A
PASSWORD: SWAP12345
IP:       192.168.4.1
TCP PORT: 4210

============================================================
BLE
============================================================

NAME:     SWAP-A
SERVICE:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX:       6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX:       6E400003-B5A3-F393-E0A9-E50E24DCCA9E

============================================================
SX1262
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
UART TO UNO Q
============================================================

TELEMETRY_SERIAL  Serial2
BAUD              115200 (matches UNO Q's D0/D1 usart1 console rate)
RX / TX PIN       GPIO16 / GPIO17
============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <RadioLib.h>
#include <esp_wifi.h>       // esp_wifi_ap_get_sta_list() -- real AP-side per-station RSSI
// ble_gap_conn_rssi() comes from NimBLEDevice.h above, which already pulls
// in host/ble_gap.h through its own conditional include path -- including
// it directly here fails, since that path isn't on the sketch's include
// search path.

// Defined here, near the top, on purpose: the Arduino IDE auto-generates
// function prototypes and inserts them ABOVE this point in the file. A
// function returning LinkStats needs the type visible before that
// auto-generated prototype, or the build fails with "'LinkStats' does not
// name a type" even though the struct is defined correctly further down.
struct LinkStats {
  bool valid = false;
  float packetLoss = 0;      // 0..1
  float successRate = 0;     // 0..1
  float avgRttMs = 0;
  float maxRttMs = 0;
  float jitterMs = 0;
  float throughputBps = 0;   // bits/sec of confirmed round-tripped payload
  float stabilityDb = 0;     // RSSI standard deviation (lower = steadier)
  uint16_t sent = 0;
  uint16_t acked = 0;
};

// Explicit forward declarations. Do not rely on the Arduino IDE's automatic prototype generator for this sketch.
void pollUartCommand();
void serviceSerialInput();
size_t sendRawToB(const String& msg);


// ============================================================
// TIMING
// ============================================================

// How often a dropped/never-established connection is retried. Not a
// give-up timeout -- protocols retry forever until a command moves them.
#define RECONNECT_RETRY_MS 3000UL

// How often Node B sends its perception data, and Node A emits the
// combined UART frame to the UNO Q.
#define PERCEPTION_INTERVAL_MS 4000UL

// How long to let a "C:" switch command actually leave the radio/socket
// before tearing the old protocol's resources down.
#define SWITCH_FLUSH_DELAY_MS 150UL

// Trial windows: how long a commanded switch gets to prove itself before
// being declared a failure and rolled back. Chosen per-protocol because
// association time differs a lot -- BLE scan+connect is the slowest,
// LoRa is the fastest since it's connectionless (see trialSucceeded()).
#define WIFI_TRIAL_MS 5000UL   // AP already up, just waiting for B's TCP connect
#define BLE_TRIAL_MS  5000UL
#define BLE_SWITCH_GUARD_MS 3000UL   // do not issue a new switch for 3 s after BLE connects
                                // startBLE(): the old forced 3s pre-scan delay was
                                // trimmed, so this window is now actually reachable.
#define LORA_TRIAL_MS 5000UL   // no AP/STA or client/server, just radio bring-up


// ============================================================
// TELEMETRY UART (Serial2 -> UNO Q, see uno_q_mcu_sketch)
// ============================================================

#define TELEMETRY_SERIAL Serial2
#define TELEMETRY_BAUD   115200
#define TELEMETRY_RX_PIN 16
#define TELEMETRY_TX_PIN 17


// ============================================================
// WIFI
// ============================================================

const char WIFI_SSID[] = "SWAP-A";
const char WIFI_PASS[] = "SWAP12345";

#define WIFI_PORT 4210

WiFiServer wifiServer(WIFI_PORT);
WiFiClient wifiClient;

bool wifiConnected = false;


// ============================================================
// BLE
// ============================================================

const char BLE_NAME[] = "SWAP-A";

#define BLE_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

NimBLEServer* bleServer = nullptr;

NimBLECharacteristic* bleRxChar = nullptr;
NimBLECharacteristic* bleTxChar = nullptr;

volatile bool bleConnected = false;
volatile uint16_t bleConnHandle = 0;
volatile uint32_t bleConnectedSinceMs = 0;  // needed for ble_gap_conn_rssi()
bool bleInitialized = false;
bool bleWanted = false;              // true only while BLE is the active protocol


// ============================================================
// LORA
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

// LoRa is connectionless. Treat the peer as connected only after a recent
// probe acknowledgement. This is telemetry for UNO Q; it never causes an
// automatic protocol switch.
bool loraPeerConnected = false;
uint32_t lastLoraAckMs = 0;
#define LORA_PEER_TIMEOUT_MS 8000UL

float loraOwnRssi = -999.0f;
float loraOwnSnr = -999.0f;
bool loraOwnCrcOk = false;
unsigned long loraOwnToaMs = 0;

// Static radio config -- fixed constants, not measured, but part of the
// LoRa perception picture per multi_radio_perception_data spec section 24.
#define LORA_TX_POWER_DBM 14
#define LORA_SF 7
#define LORA_BW_KHZ 125
#define LORA_CR 5


// ============================================================
// PROTOCOL STATE
// ============================================================

enum Protocol { WIFI_PROTOCOL = 0, BLE_PROTOCOL = 1, LORA_PROTOCOL = 2 };

// STABLE:      activeProtocol is the agreed, working link -- normal
//              operation. The only state a new switchProtocol() call is
//              accepted from.
// NEGOTIATING: a commanded switch was requested; Node A is pinging Node B
//              with an "I:<N>" intent over the CURRENT (still-good) link
//              and waiting for a "K:<N>" ack, WITHOUT having touched
//              activeProtocol yet. Nothing is torn down in this state --
//              it's purely a reachability check on Node B before either
//              side commits to anything.
// TRIALING:    the ack came back, the actual "C:<N>" switch command went
//              out, and both boards have torn down the old protocol and
//              brought up the new one. activeProtocol is unproven until
//              it either shows connected within its trial window
//              (-> STABLE) or the window expires (-> auto-rollback to
//              previousProtocol, -> STABLE). Node A reports the outcome
//              to UNO Q either way; UNO Q decides what to try next.
enum SwitchState { STATE_STABLE = 0, STATE_NEGOTIATING = 1, STATE_TRIALING = 2, STATE_RECOVERING = 3 };

// How often to (re-)send the "I:<N>" intent while waiting for Node B's
// ack, and how many times to try before giving up on this switch.
#define INTENT_RETRY_MS      300UL
#define INTENT_MAX_ATTEMPTS  5   // ~1.5s worst case before giving up

void switchProtocol(Protocol target);
const char* protocolName(Protocol p);

Protocol pendingTarget = LORA_PROTOCOL;   // valid only while NEGOTIATING
uint8_t negotiationAttempts = 0;
uint32_t lastIntentSentMs = 0;
volatile bool pendingAckReceived = false; // set by handleIncomingFromB on "K:<N>" match
int deferredSwitchTarget = -1;
bool deferredSwitchPending = false;

// Boot protocol is LoRa, not Wi-Fi: it's the one link that doesn't need a
// peer already listening on a specific SSID/GATT service to come up, so
// it's the safest thing to establish first. UNO Q only starts commanding
// Wi-Fi/BLE trials once it sees LoRa connected in the telemetry stream --
// that's a UNO Q/backend decision, not something this firmware enforces.
Protocol activeProtocol = LORA_PROTOCOL;
// previousProtocol always holds the last protocol that was actually
// STABLE (proven or the LoRa floor) -- see switchProtocol()'s comment.
// Self-referential at boot: LoRa has nothing below it to roll back to.
Protocol previousProtocol = LORA_PROTOCOL;
uint32_t lastRetryAttempt = 0;

SwitchState switchState = STATE_STABLE;
uint32_t trialStartMs = 0;
uint32_t recoveryStartMs = 0;
Protocol recoveryProtocol = LORA_PROTOCOL;
bool recoveryWasConnected = false;
bool linkWasEverConnected = false;


// Node B's last-reported perception data, relayed to the UNO Q.
bool haveNodeBData = false;
float nodeBRssi = -999.0f;
float nodeBSnr = -999.0f;
int nodeBChannel = -1;         // WiFi only
bool nodeBCrcOk = false;       // LoRa only
unsigned long nodeBToaMs = 0;  // LoRa only


// ============================================================
// BLE SERVER CALLBACK
// ============================================================

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    bleConnected = true;
    bleConnHandle = info.getConnHandle();
    bleConnectedSinceMs = millis();  // saved so we can read real RSSI
    Serial.println("[BLE] CONNECTED");
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
    bleConnectedSinceMs = 0;

    // Do not resurrect BLE when the state machine intentionally switched
    // away from it. Re-advertise only while BLE is still the active protocol.
    if (bleWanted) {
      Serial.println("[BLE] DISCONNECTED -- RE-ADVERTISING");
      NimBLEDevice::startAdvertising();
    } else {
      Serial.println("[BLE] DISCONNECTED");
    }
  }
};


// ============================================================
// INCOMING MESSAGE HANDLER (shared by WiFi/BLE/LoRa)
// ============================================================

// Splits "a,b,c,..." on commas into out[], returns how many fields found
// (capped at maxFields). Small helper so each protocol's "D:" payload can
// carry a different field layout without one-off parsing code per case.
int splitFields(const String& s, String out[], int maxFields) {
  int count = 0;
  int start = 0;
  for (int i = 0; i <= (int)s.length() && count < maxFields; i++) {
    if (i == (int)s.length() || s[i] == ',') {
      out[count++] = s.substring(start, i);
      start = i + 1;
    }
  }
  return count;
}

// ============================================================
// ACTIVE LINK PROBING (spec section 28)
// ============================================================
//
// Node A sends "P:<seq>"; Node B echoes "R:<seq>" the moment it arrives.
// The round trip is what makes packet loss, RTT, jitter, throughput and
// stability real measurements instead of blank fields. A fixed ring buffer
// holds the last PROBE_WINDOW probes -- O(1) to record, O(window) to
// summarise once per UART frame, no dynamic allocation.

#define PROBE_WINDOW 20
#define PROBE_TIMEOUT_MS 2500UL

struct ProbeSlot {
  uint32_t seq;
  uint32_t txMs;
  uint32_t rttMs;
  bool used;
  bool acked;
};

ProbeSlot probes[PROBE_WINDOW];
uint8_t probeHead = 0;
uint32_t probeSeq = 0;
uint32_t probeBytesAcked = 0;   // payload bytes confirmed round-tripped
uint32_t probeWindowStartMs = 0;

// Rolling RSSI history, for the stability (standard deviation) metric.
float rssiHistory[PROBE_WINDOW];
uint8_t rssiCount = 0;
uint8_t rssiHead = 0;

// LoRa airtime is far higher than WiFi/BLE, so probing it at the same rate
// would dominate the duty cycle for no extra information.
uint32_t probeIntervalMs() {
  return (activeProtocol == LORA_PROTOCOL) ? 3000UL : 1000UL;
}

void resetProbeStats() {
  for (uint8_t i = 0; i < PROBE_WINDOW; i++) {
    probes[i].used = false;
    probes[i].acked = false;
  }
  probeHead = 0;
  probeBytesAcked = 0;
  probeWindowStartMs = millis();
  rssiCount = 0;
  rssiHead = 0;
}

void sendProbe() {
  String msg = "P:" + String(probeSeq);
  size_t sent = sendRawToB(msg);
  if (sent == 0) {
    return;  // link down -- don't record a probe that never left
  }

  probes[probeHead].seq = probeSeq;
  probes[probeHead].txMs = millis();
  probes[probeHead].rttMs = 0;
  probes[probeHead].used = true;
  probes[probeHead].acked = false;
  probeHead = (probeHead + 1) % PROBE_WINDOW;
  probeSeq++;
}

void onProbeAck(uint32_t seq, size_t replyBytes) {
  loraPeerConnected = (activeProtocol == LORA_PROTOCOL);
  lastLoraAckMs = millis();

  for (uint8_t i = 0; i < PROBE_WINDOW; i++) {
    if (probes[i].used && !probes[i].acked && probes[i].seq == seq) {
      probes[i].acked = true;
      probes[i].rttMs = millis() - probes[i].txMs;
      // Round trip: the probe out plus the reply back.
      probeBytesAcked += replyBytes + (8 + String(seq).length());
      return;
    }
  }
}

void recordRssiSample(float rssi) {
  if (rssi <= -900.0f) {
    return;  // sentinel, not a reading
  }
  rssiHistory[rssiHead] = rssi;
  rssiHead = (rssiHead + 1) % PROBE_WINDOW;
  if (rssiCount < PROBE_WINDOW) rssiCount++;
}

LinkStats computeLinkStats() {
  LinkStats s;
  const uint32_t now = millis();

  uint16_t resolved = 0, acked = 0;
  float rttSum = 0;
  float rtts[PROBE_WINDOW];
  uint8_t rttN = 0;

  for (uint8_t i = 0; i < PROBE_WINDOW; i++) {
    if (!probes[i].used) continue;
    if (probes[i].acked) {
      resolved++;
      acked++;
      rttSum += probes[i].rttMs;
      if (probes[i].rttMs > s.maxRttMs) s.maxRttMs = probes[i].rttMs;
      rtts[rttN++] = probes[i].rttMs;
    } else if (now - probes[i].txMs > PROBE_TIMEOUT_MS) {
      resolved++;  // timed out == lost
    }
    // Still-pending probes are deliberately excluded: counting them as lost
    // would inflate loss every time a probe is simply in flight.
  }

  if (resolved == 0) {
    return s;  // nothing conclusive yet
  }

  s.valid = true;
  s.sent = resolved;
  s.acked = acked;
  s.successRate = (float)acked / (float)resolved;
  s.packetLoss = 1.0f - s.successRate;
  s.avgRttMs = (rttN > 0) ? (rttSum / rttN) : 0;

  // Jitter: mean absolute difference between consecutive RTT samples.
  if (rttN >= 2) {
    float diffSum = 0;
    for (uint8_t i = 1; i < rttN; i++) {
      diffSum += fabsf(rtts[i] - rtts[i - 1]);
    }
    s.jitterMs = diffSum / (rttN - 1);
  }

  const uint32_t elapsed = now - probeWindowStartMs;
  if (elapsed > 0) {
    s.throughputBps = (probeBytesAcked * 8.0f * 1000.0f) / (float)elapsed;
  }

  // Stability = standard deviation of recent RSSI.
  if (rssiCount >= 2) {
    float mean = 0;
    for (uint8_t i = 0; i < rssiCount; i++) mean += rssiHistory[i];
    mean /= rssiCount;
    float var = 0;
    for (uint8_t i = 0; i < rssiCount; i++) {
      const float d = rssiHistory[i] - mean;
      var += d * d;
    }
    s.stabilityDb = sqrtf(var / rssiCount);
  }

  return s;
}


// From Node B: "R:<seq>" (probe reply), "D:..." (perception data),
// "K:<N>" (ack to our "I:<N>" switch intent), or the boot handshake
// "BOOT:B:LORA". There is no "C:" case here: Node A sends switch
// commands, it never receives them.
void handleIncomingFromB(const String& msg) {
  // Any valid message from Node B proves that the current link is alive.
  if (activeProtocol == LORA_PROTOCOL) {
    loraPeerConnected = true;
    lastLoraAckMs = millis();
  }

  if (msg.startsWith("R:")) {
    onProbeAck((uint32_t)msg.substring(2).toInt(), msg.length());
    return;
  }

  if (msg.startsWith("K:")) {
    int acked = msg.substring(2).toInt();
    if (switchState == STATE_NEGOTIATING && acked == (int)pendingTarget) {
      pendingAckReceived = true;
    }
    return;
  }

  // Node B cannot talk to UNO Q directly; it announces its boot over LoRa
  // and Node A relays that initialization event over UART.
  if (msg == "BOOT:B:LORA") {
    TELEMETRY_SERIAL.println("{\"event\":\"node_initialized\",\"node\":\"B\",\"initialized\":true,\"boot_protocol\":\"lora\"}");
    Serial.println("[BOOT] Node B initialized on LoRa -> UNO Q");

    // Acknowledge the boot announcement so Node B can stop retransmitting.
    sendRawToB("BOOT_ACK:B");
    return;
  }

  if (!msg.startsWith("D:")) {
    return;
  }

  String fields[4];
  int n = splitFields(msg.substring(2), fields, 4);
  if (n < 1) {
    return;
  }

  nodeBRssi = fields[0].toFloat();

  switch (activeProtocol) {
    case WIFI_PROTOCOL:
      // "D:<rssi>,<channel>"
      nodeBChannel = (n >= 2) ? fields[1].toInt() : -1;
      break;

    case BLE_PROTOCOL:
      // "D:<rssi>" -- nothing else cheaply available from the client API.
      break;

    case LORA_PROTOCOL:
      // "D:<rssi>,<snr>,<crc_ok 0|1>,<time_on_air_ms>"
      nodeBSnr = (n >= 2) ? fields[1].toFloat() : -999.0f;
      nodeBCrcOk = (n >= 3) ? (fields[2].toInt() != 0) : false;
      nodeBToaMs = (n >= 4) ? (unsigned long)fields[3].toInt() : 0;
      break;
  }

  haveNodeBData = true;
}


// ============================================================
// BLE RX CALLBACK
// ============================================================

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    std::string value = characteristic->getValue();

    if (value.empty()) {
      return;
    }

    handleIncomingFromB(String(value.c_str()));
  }
};


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
  Serial.println("[WIFI] START (AP, waiting for Node B)");

  wifiConnected = false;

  WiFi.mode(WIFI_MODE_NULL);
  delay(30);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(WIFI_SSID, WIFI_PASS, 6, false, 2);
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );

  wifiServer.begin();
  wifiServer.setNoDelay(true);

  Serial.print("[WIFI] IP: ");
  Serial.println(WiFi.softAPIP());
}

void stopWiFi() {
  Serial.println("[WIFI] OFF");

  wifiClient.stop();
  wifiServer.end();

  wifiConnected = false;

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_MODE_NULL);

  delay(30);
}

void serviceWiFi() {
  if (!wifiConnected) {
    WiFiClient incoming = wifiServer.available();

    if (incoming) {
      wifiClient = incoming;
      wifiClient.setNoDelay(true);
      wifiConnected = true;
      Serial.println("[WIFI] NODE B CONNECTED");
    }
    return;
  }

  while (wifiClient.available()) {
    String line = wifiClient.readStringUntil('\n');
    line.trim();
    if (line.length()) {
      handleIncomingFromB(line);
    }
  }

  // Connection dropped while Wi-Fi was stable. Keep Wi-Fi alive during the
  // 5-second recovery window; the recovery state machine decides whether to
  // remain on Wi-Fi or roll back to previousProtocol.
  if (!wifiClient.connected()) {
    wifiClient.stop();
    wifiConnected = false;
    Serial.println("[WIFI] Node B disconnected -- waiting for reconnect");
  }
}


// ============================================================
// BLE START / STOP / SERVICE
// ============================================================

void startBLE() {
  Serial.println("[BLE] START");

  bleWanted = true;
  bleConnected = false;
  bleConnectedSinceMs = 0;

  // Initialize the NimBLE host/controller exactly once. Switching protocols
  // must NOT deinit/reinit NimBLE: that is the wrong lifecycle for a link
  // that is expected to come back repeatedly.
  if (!bleInitialized) {
    if (!NimBLEDevice::init(BLE_NAME)) {
      Serial.println("[BLE] INIT FAILED");
      bleWanted = false;
      return;
    }

    NimBLEDevice::setPower(9);

    bleServer = NimBLEDevice::createServer();
    if (bleServer == nullptr) {
      Serial.println("[BLE] SERVER CREATE FAILED");
      bleWanted = false;
      return;
    }

    bleServer->setCallbacks(new ServerCallbacks());

    NimBLEService* service = bleServer->createService(BLE_SERVICE);

    bleRxChar = service->createCharacteristic(
      BLE_RX,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    bleRxChar->setCallbacks(new RxCallbacks());

    bleTxChar = service->createCharacteristic(
      BLE_TX,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    service->start();

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE);
    advertising->setName(BLE_NAME);

    bleInitialized = true;
  }

  NimBLEDevice::startAdvertising();
  Serial.println("[BLE] ADVERTISING");
}

void stopBLE() {
  Serial.println("[BLE] OFF");

  // Mark BLE unwanted BEFORE disconnecting. Otherwise the disconnect callback
  // can immediately restart advertising while another protocol is starting.
  bleWanted = false;

  NimBLEDevice::stopAdvertising();

  if (bleServer != nullptr && bleConnected) {
    bleServer->disconnect(bleConnHandle);
  }

  bleConnected = false;
  bleConnHandle = 0;
  bleConnectedSinceMs = 0;

  // Keep the NimBLE host/controller initialized. We only stop the BLE link;
  // the stack itself is reused on the next BLE trial.
  delay(50);
}

void serviceBLE() {
  // Connection state + incoming data are both driven by callbacks above.
  // Nothing to poll here.
}


// ============================================================
// LORA START / STOP / SERVICE
// ============================================================

void startLoRa() {
  Serial.println("[LORA] START");

  loraReady = false;
  loraReceived = false;
  loraPeerConnected = false;
  lastLoraAckMs = 0;

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
  loraPeerConnected = false;
  lastLoraAckMs = 0;
}

void serviceLoRa() {
  if (!loraReady) {
    // Retry init periodically rather than giving up.
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

  // CRC failures still land here with a non-NONE result -- readData()
  // returns RADIOLIB_ERR_CRC_MISMATCH rather than silently dropping the
  // packet, so we can count them instead of only ever seeing successes.
  loraOwnCrcOk = (result == RADIOLIB_ERR_NONE);

  if (result == RADIOLIB_ERR_NONE) {
    loraOwnRssi = radio.getRSSI();
    loraOwnSnr = radio.getSNR();
    // getTimeOnAir() returns MICROseconds (confirmed in SX126x.h) -- convert
    // to ms so the reported field matches its name.
    loraOwnToaMs = radio.getTimeOnAir(data.length()) / 1000UL;

    if (data.length() > 0) {
      handleIncomingFromB(data);
    }
  }
}


// ============================================================
// SEND SWITCH COMMAND TO NODE B (before Node A itself switches)
// ============================================================

// Single outbound path to Node B over whichever protocol is active.
// Returns the number of payload bytes actually put on the air (0 if the
// link wasn't up), which the throughput estimate needs.
size_t sendRawToB(const String& msg) {
  switch (activeProtocol) {
    case WIFI_PROTOCOL:
      if (wifiConnected && wifiClient.connected()) {
        wifiClient.print(msg);
        wifiClient.print('\n');
        return msg.length();
      }
      break;

    case BLE_PROTOCOL:
      if (bleConnected && bleTxChar != nullptr) {
        bleTxChar->setValue(msg.c_str());
        bleTxChar->notify();
        return msg.length();
      }
      break;

    case LORA_PROTOCOL:
      if (loraReady) {
        String tx = msg;              // transmit() takes a non-const String&
        radio.transmit(tx);           // blocking
        radio.startReceive();         // must re-arm RX after every TX
        return msg.length();
      }
      break;
  }
  return 0;
}

void sendSwitchCommandToB(Protocol target) {
  String cmd = "C:" + String((int)target);

  Serial.print("[SWITCH] Telling Node B -> ");
  Serial.println(cmd);

  sendRawToB(cmd);
  delay(SWITCH_FLUSH_DELAY_MS);
}


// ============================================================
// PROTOCOL SWITCH (command-driven only)
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
  lastRetryAttempt = millis();
}

// Stale perception/probe data from the old protocol shouldn't be reported
// under the new one's schema -- shared by every path that brings up a new
// active protocol (commanded switch, manual override, or auto-rollback).
void resetPeerDataForNewProtocol() {
  haveNodeBData = false;
  loraPeerConnected = false;
  lastLoraAckMs = 0;
  nodeBSnr = -999.0f;
  nodeBChannel = -1;
  nodeBCrcOk = false;
  nodeBToaMs = 0;
  resetProbeStats();
}

uint32_t trialWindowMs(Protocol p) {
  switch (p) {
    case WIFI_PROTOCOL: return WIFI_TRIAL_MS;
    case BLE_PROTOCOL:  return BLE_TRIAL_MS;
    case LORA_PROTOCOL: return LORA_TRIAL_MS;
  }
  return WIFI_TRIAL_MS;
}

// Whether the CURRENTLY active protocol counts as "proven" for trial
// purposes. WiFi/BLE need an actual peer connection (mirrors
// reportLinkConnected()). LoRa is connectionless -- there's no AP/STA or
// client/server handshake to wait on, so radio bring-up succeeding is the
// whole bar, exactly as discussed: LoRa is the safe harbor, not a peer.
bool trialSucceeded() {
  switch (activeProtocol) {
    case WIFI_PROTOCOL: return wifiConnected;
    case BLE_PROTOCOL:  return bleConnected;
    case LORA_PROTOCOL:
      return loraReady &&
             loraPeerConnected &&
             (millis() - lastLoraAckMs <= LORA_PEER_TIMEOUT_MS);
  }
  return false;
}

// Tells UNO Q the outcome of a commanded switch. UNO Q always picks what
// happens next (retry, try a different protocol, or leave it) -- Node A
// only ever reports and, on failure, self-heals back to the last known
// good protocol so the link isn't left down while UNO Q decides.
void emitSwitchResult(bool success, Protocol attempted, Protocol current) {
  char json[160];
  snprintf(json, sizeof(json),
    "{\"cmd\":\"switch_result\",\"status\":\"%s\",\"attempted\":\"%s\",\"active\":\"%s\"}",
    success ? "success" : "failed",
    protocolName(attempted),
    protocolName(current));
  TELEMETRY_SERIAL.println(json);
  Serial.print("[UART->UNOQ] ");
  Serial.println(json);
}

// Tells UNO Q a force_protocol command was refused, and why: either a
// switch was already in flight (negotiating or trialing), or Node B never
// acked the intent to switch (see serviceNegotiation()). Either way UNO Q
// needs to know the command didn't take, so it doesn't assume a switch
// happened that never did.
void emitSwitchRejected(Protocol requested, const char* reason) {
  char json[192];
  snprintf(json, sizeof(json),
    "{\"cmd\":\"switch_result\",\"status\":\"rejected\",\"requested\":\"%s\",\"reason\":\"%s\",\"active\":\"%s\"}",
    protocolName(requested),
    reason,
    protocolName(activeProtocol));
  TELEMETRY_SERIAL.println(json);
  Serial.print("[UART->UNOQ] ");
  Serial.println(json);
}

void sendIntent(Protocol target) {
  String msg = "I:" + String((int)target);
  sendRawToB(msg);
  lastIntentSentMs = millis();
  negotiationAttempts++;
  Serial.print("[NEGOTIATE] Intent -> Node B, protocol ");
  Serial.print((int)target);
  Serial.print(" (attempt ");
  Serial.print(negotiationAttempts);
  Serial.println(")");
}

bool activeProtocolConnected() {
  switch (activeProtocol) {
    case WIFI_PROTOCOL: return wifiConnected;
    case BLE_PROTOCOL:  return bleConnected;
    case LORA_PROTOCOL:
      return loraPeerConnected && (millis() - lastLoraAckMs <= LORA_PEER_TIMEOUT_MS);
  }
  return false;
}

void beginLinkRecovery() {
  if (switchState != STATE_STABLE) return;

  if (activeProtocol == LORA_PROTOCOL) {
    if (loraPeerConnected && (millis() - lastLoraAckMs <= LORA_PEER_TIMEOUT_MS)) return;
  } else if (activeProtocolConnected()) {
    return;
  }

  recoveryProtocol = activeProtocol;
  recoveryStartMs = millis();
  recoveryWasConnected = true;
  switchState = STATE_RECOVERING;
  Serial.print("[RECOVERY] ");
  Serial.print(protocolName(activeProtocol));
  Serial.println(" link lost -- 5-second recovery window started");
}

void serviceLinkRecovery() {
  if (switchState != STATE_RECOVERING) return;

  if (activeProtocolConnected()) {
    switchState = STATE_STABLE;
    recoveryWasConnected = false;
    Serial.print("[RECOVERY] ");
    Serial.print(protocolName(activeProtocol));
    Serial.println(" link recovered");
    return;
  }

  if (millis() - recoveryStartMs < 5000UL) return;

  Protocol failed = activeProtocol;
  Serial.print("[RECOVERY] ");
  Serial.print(protocolName(failed));
  Serial.print(" did not recover in 5 s -- rolling back to ");
  Serial.println(protocolName(previousProtocol));

  stopActiveProtocol();
  activeProtocol = previousProtocol;
  startActiveProtocol();
  resetPeerDataForNewProtocol();
  linkWasEverConnected = false;
  switchState = STATE_STABLE;
  recoveryWasConnected = false;
}

// Commanded switch (from UNO Q via pollUartCommand). Does NOT touch
// activeProtocol directly -- it only starts negotiation. The actual
// switch + trial only happens once Node B acks, in serviceNegotiation().
//
// Locked out while a switch is already in flight (NEGOTIATING or
// TRIALING): previousProtocol is only trustworthy as a rollback target if
// it was itself STABLE (i.e. actually proven, or the LoRa floor) when
// this switch started. Accepting a second command mid-flight would
// overwrite previousProtocol with a protocol that was never proven
// either, so a later rollback could land on an untested protocol instead
// of the last genuinely trusted one. UNO Q must wait for a switch_result
// (success, failed, or rejected) before sending another force_protocol.
void switchProtocol(Protocol target) {
  if (target == activeProtocol) {
    return;
  }

  if (switchState != STATE_STABLE) {
    Serial.print("[SWITCH] Rejected -- state ");
    Serial.print((int)switchState);
    Serial.println(" is not stable");
    emitSwitchRejected(target, switchState == STATE_RECOVERING ? "link_recovery_in_progress" : "switch_in_progress");
    return;
  }

  // BLE needs a short settling period after a successful connection before
  // another protocol-switch transaction is allowed. This protects the first
  // post-connect command from being sent while the GATT link is still settling.
  if (activeProtocol == BLE_PROTOCOL && bleConnected) {
    uint32_t bleAge = millis() - bleConnectedSinceMs;
    if (bleAge < BLE_SWITCH_GUARD_MS) {
      // Do not lose a command that arrived immediately after BLE connected.
      // Hold the latest requested target and execute it after the 3-second
      // BLE settling guard, provided the link is still stable.
      deferredSwitchTarget = (int)target;
      deferredSwitchPending = true;
      Serial.print("[SWITCH] Deferred -- BLE connected only ");
      Serial.print(bleAge);
      Serial.println(" ms ago; waiting for 3-second guard");
      return;
    }
  }

  // Phase 1: negotiate. Ping Node B over the CURRENT (still-good) link
  // and wait for an ack before committing to anything -- see the
  // NEGOTIATING state comment. Nobody tears anything down yet.
  pendingTarget = target;
  negotiationAttempts = 0;
  pendingAckReceived = false;
  switchState = STATE_NEGOTIATING;
  sendIntent(pendingTarget);
}

void serviceDeferredSwitch() {
  if (!deferredSwitchPending) return;

  if (switchState != STATE_STABLE) {
    deferredSwitchPending = false;
    deferredSwitchTarget = -1;
    return;
  }

  if (activeProtocol != BLE_PROTOCOL || !bleConnected) {
    // BLE disappeared during the guard. Do not apply a stale switch command
    // after recovery/rollback changes the protocol context.
    deferredSwitchPending = false;
    deferredSwitchTarget = -1;
    return;
  }

  if (millis() - bleConnectedSinceMs < BLE_SWITCH_GUARD_MS) return;

  int target = deferredSwitchTarget;
  deferredSwitchPending = false;
  deferredSwitchTarget = -1;

  if (target >= 0 && target <= 2) {
    switchProtocol((Protocol)target);
  }
}

// Called every loop() while NEGOTIATING. Non-blocking: resends the intent
// on a timer, and either commits the switch once Node B acks, or gives up
// and reports rejection to UNO Q once out of attempts.
void serviceNegotiation() {
  if (switchState != STATE_NEGOTIATING) {
    return;
  }

  if (pendingAckReceived) {
    Serial.print("[NEGOTIATE] Node B acked -- committing to protocol ");
    Serial.println((int)pendingTarget);

    // Phase 2: commit. This is the same "C:" flow as before, just gated
    // on proof Node B is actually reachable right now.
    sendSwitchCommandToB(pendingTarget);

    previousProtocol = activeProtocol;
    stopActiveProtocol();
    activeProtocol = pendingTarget;
    startActiveProtocol();
    resetPeerDataForNewProtocol();

    switchState = STATE_TRIALING;
    trialStartMs = millis();
    pendingAckReceived = false;

    Serial.print("[SWITCH] Trialing protocol ");
    Serial.println((int)activeProtocol);
    return;
  }

  if (millis() - lastIntentSentMs < INTENT_RETRY_MS) {
    return;
  }

  if (negotiationAttempts >= INTENT_MAX_ATTEMPTS) {
    Serial.print("[NEGOTIATE] Gave up -- Node B never acked protocol ");
    Serial.println((int)pendingTarget);
    switchState = STATE_STABLE;
    emitSwitchRejected(pendingTarget, "no_peer_ack");
    return;
  }

  sendIntent(pendingTarget);
}

// USB-serial W/B/L override used to bypass negotiation entirely -- fire
// "C:<N>" once, wait a fixed 150ms, tear down. That's the exact race
// condition negotiation was built to close on the commanded path: no
// confirmation Node B actually received the notification/packet before
// the old link is torn down (BLE notify delivery timing depends on the
// negotiated connection interval, which can exceed 150ms). Rather than
// keep two switch mechanisms -- one proven, one fragile -- W/B/L now goes
// through the same switchProtocol() negotiate-then-commit path as a real
// UNO Q command. Costs up to ~1.5s instead of being instant, but it's the
// same code path already relied on for the real flow, so there's nothing
// separate left to go wrong. (This also means a manual switch now emits
// a switch_result to UNO Q -- harmless, and arguably useful for seeing
// bench tests show up in the same telemetry stream.)

// Called every loop(). While TRIALING, does nothing until the current
// protocol's trial window elapses, then either confirms the switch
// (STABLE) or rolls back to previousProtocol and reports the failure.
// Node B never gets an explicit rollback message here -- it can't, there's
// no live link during a failed trial to send one over. Instead it runs the
// identical timeout independently (same trial windows, started within
// SWITCH_FLUSH_DELAY_MS of Node A's), so both boards land back on
// previousProtocol on their own and resync without needing to talk.
void checkTrialResult() {
  if (switchState != STATE_TRIALING) {
    return;
  }
  if (millis() - trialStartMs < trialWindowMs(activeProtocol)) {
    return;
  }

  Protocol attempted = activeProtocol;

  if (trialSucceeded()) {
    linkWasEverConnected = true;
    switchState = STATE_STABLE;
    Serial.print("[TRIAL] SUCCESS on protocol ");
    Serial.println((int)attempted);
    emitSwitchResult(true, attempted, activeProtocol);
    return;
  }

  Serial.print("[TRIAL] FAILED on protocol ");
  Serial.print((int)attempted);
  Serial.print(" -- rolling back to ");
  Serial.println((int)previousProtocol);

  stopActiveProtocol();
  activeProtocol = previousProtocol;
  startActiveProtocol();
  resetPeerDataForNewProtocol();
  linkWasEverConnected = false;
  switchState = STATE_STABLE;

  emitSwitchResult(false, attempted, activeProtocol);
}


// ============================================================
// UART COMMAND FROM UNO Q
// ============================================================

String uartLineBuf;

bool extractProtocolFromCommand(const String& line, int& protocol) {
  if (line.indexOf("\"cmd\":\"force_protocol\"") < 0) return false;
  int key = line.indexOf("\"protocol\"");
  if (key < 0) return false;
  int colon = line.indexOf(':', key);
  if (colon < 0) return false;
  protocol = line.substring(colon + 1).toInt();
  return protocol >= 0 && protocol <= 2;
}

void pollUartCommand() {
  while (TELEMETRY_SERIAL.available()) {
    char c = (char)TELEMETRY_SERIAL.read();
    if (c == '\n' || c == '\r') {
      if (uartLineBuf.length()) {
        int p = -1;
        if (extractProtocolFromCommand(uartLineBuf, p)) {
          switchProtocol((Protocol)p);
        }
        uartLineBuf = "";
      }
    } else if (uartLineBuf.length() < 160) {
      uartLineBuf += c;
    }
  }
}


// ============================================================
// EMIT COMBINED TELEMETRY TO UNO Q
// ============================================================

bool reportLinkConnected() {
  switch (activeProtocol) {
    case WIFI_PROTOCOL:
      return wifiConnected;
    case BLE_PROTOCOL:
      return bleConnected;
    case LORA_PROTOCOL:
      return loraReady &&
             loraPeerConnected &&
             (millis() - lastLoraAckMs <= LORA_PEER_TIMEOUT_MS);
  }
  return false;
}

const char* protocolName(Protocol p) {
  switch (p) {
    case WIFI_PROTOCOL: return "wifi";
    case BLE_PROTOCOL:  return "ble";
    case LORA_PROTOCOL: return "lora";
  }
  return "wifi";
}

// Real AP-side RSSI of the connected station (Node B). Arduino's
// WiFi.RSSI() only works in station mode, which is why this used to report
// the -999 "unavailable" sentinel -- esp_wifi_ap_get_sta_list() is the
// AP-side equivalent and gives a genuine per-station dBm reading.
float readOwnWifiRssi() {
  wifi_sta_list_t staList;
  if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK || staList.num == 0) {
    return -999.0f;  // genuinely nothing connected, not a measurement failure
  }
  return (float)staList.sta[0].rssi;
}

// Real server-side RSSI for the active BLE connection. NimBLE's
// NimBLEServer class has no getRssi() the way NimBLEClient does, but the
// underlying host API does -- this is that call, using the conn handle
// saved in ServerCallbacks::onConnect().
float readOwnBleRssi() {
  if (!bleConnected) {
    return -999.0f;
  }
  int8_t rssi = 0;
  if (ble_gap_conn_rssi(bleConnHandle, &rssi) != 0) {
    return -999.0f;
  }
  return (float)rssi;
}

void emitUartFrame() {
  float ownRssi = -999.0f;
  float ownSnr = -999.0f;

  switch (activeProtocol) {
    case WIFI_PROTOCOL: ownRssi = readOwnWifiRssi(); break;
    case BLE_PROTOCOL:  ownRssi = readOwnBleRssi(); break;
    case LORA_PROTOCOL: ownRssi = loraOwnRssi; ownSnr = loraOwnSnr; break;
  }

  recordRssiSample(ownRssi);
  LinkStats st = computeLinkStats();

  char json[768];
  int n = snprintf(json, sizeof(json),
    "{\"link_state\":\"%s\",\"connected\":%s,\"timestamp\":%.3f,\"link_rssi\":%.1f",
    protocolName(activeProtocol),
    reportLinkConnected() ? "true" : "false",
    millis() / 1000.0,
    ownRssi);

  if (activeProtocol == WIFI_PROTOCOL) {
    n += snprintf(json+n, sizeof(json)-n, ",\"channel\":%d", WiFi.channel());
  }

  if (activeProtocol == LORA_PROTOCOL) {
    n += snprintf(json+n, sizeof(json)-n,
      ",\"link_snr\":%.1f,\"crc_ok\":%s,\"time_on_air_ms\":%lu,\"tx_power_dbm\":%d,\"spreading_factor\":%d,\"bandwidth_khz\":%d,\"coding_rate\":%d",
      ownSnr, loraOwnCrcOk ? "true" : "false", loraOwnToaMs,
      LORA_TX_POWER_DBM, LORA_SF, LORA_BW_KHZ, LORA_CR);
  }

  if (haveNodeBData) {
    n += snprintf(json+n, sizeof(json)-n, ",\"node_b_telemetry\":{\"rssi\":%.1f", nodeBRssi);
    if (activeProtocol == WIFI_PROTOCOL && nodeBChannel >= 0)
      n += snprintf(json+n, sizeof(json)-n, ",\"channel\":%d", nodeBChannel);
    if (activeProtocol == LORA_PROTOCOL)
      n += snprintf(json+n, sizeof(json)-n, ",\"snr\":%.1f,\"crc_ok\":%s,\"time_on_air_ms\":%lu", nodeBSnr, nodeBCrcOk ? "true" : "false", nodeBToaMs);
    n += snprintf(json+n, sizeof(json)-n, "}");
  }

  if (st.valid) {
    n += snprintf(json+n, sizeof(json)-n,
      ",\"packet_loss\":%.4f,\"packet_success_rate\":%.4f,\"rtt_ms\":%.2f,\"latency_ms\":%.2f,\"max_rtt_ms\":%.2f,\"jitter_ms\":%.2f,\"throughput_bps\":%.2f,\"stability_db\":%.2f,\"probes_sent\":%u,\"probes_acked\":%u",
      st.packetLoss, st.successRate, st.avgRttMs, st.avgRttMs / 2.0f, st.maxRttMs, st.jitterMs, st.throughputBps, st.stabilityDb, st.sent, st.acked);
  }

  snprintf(json+n, sizeof(json)-n, "}");
  TELEMETRY_SERIAL.println(json);
  Serial.print("[UART->UNOQ] ");
  Serial.println(json);
}


// ============================================================
// SERIAL (manual test messages, unchanged from before)
// ============================================================

void serviceSerialInput() {
  if (!Serial.available()) return;

  String message = Serial.readStringUntil('\n');
  message.trim();
  if (!message.length()) return;

  // MASTER USB override: W=WiFi, B=BLE, L=LoRa. Goes through the same
  // negotiate-then-commit path as a real UNO Q command (see the comment
  // above switchProtocol()) -- not instant, but reliable.
  if (message.length() == 1) {
    char c = message.charAt(0);
    if (c == 'W' || c == 'w') {
      Serial.println("[MANUAL] W -> WiFi");
      switchProtocol(WIFI_PROTOCOL);
      return;
    }
    if (c == 'B' || c == 'b') {
      Serial.println("[MANUAL] B -> BLE");
      switchProtocol(BLE_PROTOCOL);
      return;
    }
    if (c == 'L' || c == 'l') {
      Serial.println("[MANUAL] L -> LoRa");
      switchProtocol(LORA_PROTOCOL);
      return;
    }
  }

  // Any other USB serial line is treated as a test payload.
  switch (activeProtocol) {
    case WIFI_PROTOCOL:
      if (wifiConnected && wifiClient.connected()) {
        wifiClient.print(message);
        wifiClient.print('\n');
      }
      break;
    case BLE_PROTOCOL:
      if (bleConnected && bleTxChar != nullptr) {
        bleTxChar->setValue(message.c_str());
        bleTxChar->notify();
      }
      break;
    case LORA_PROTOCOL:
      if (loraReady) {
        radio.transmit(message);
        radio.startReceive();
      }
      break;
  }
}


// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  TELEMETRY_SERIAL.begin(TELEMETRY_BAUD, SERIAL_8N1, TELEMETRY_RX_PIN, TELEMETRY_TX_PIN);

  Serial.println();
  Serial.println("================================");
  Serial.println("   SWAP NODE A - COMMAND DRIVEN");
  Serial.println("================================");

  resetProbeStats();
  startActiveProtocol();

  // Node A has a direct UART path to UNO Q, so announce firmware/node
  // initialization immediately. The boot protocol is LoRa.
  TELEMETRY_SERIAL.println("{\"event\":\"node_initialized\",\"node\":\"A\",\"initialized\":true,\"boot_protocol\":\"lora\"}");
  Serial.println("[BOOT] Node A initialized on LoRa -> UNO Q");
}

void loop() {
  pollUartCommand();
  serviceSerialInput();
  serviceNegotiation();
  serviceDeferredSwitch();

  // Service the active protocol before evaluating the trial deadline.
  // This lets a connection callback or a packet arriving at the boundary
  // prove the link before the 5-second rollback check runs.
  switch (activeProtocol) {
    case WIFI_PROTOCOL: serviceWiFi(); break;
    case BLE_PROTOCOL:  serviceBLE();  break;
    case LORA_PROTOCOL: serviceLoRa(); break;
  }

  // A link can fail after it was already proven stable. That event gets the
  // same 5-second recovery semantics as a commanded switch: recover the same
  // protocol if possible; otherwise roll back to the last trusted protocol.
  if (switchState == STATE_STABLE) {
    if (activeProtocolConnected()) {
      linkWasEverConnected = true;
    } else if (linkWasEverConnected) {
      beginLinkRecovery();
    }
  }
  serviceLinkRecovery();
  checkTrialResult();

  // Active probe loop -- the source of RTT/loss/jitter/throughput.
  static uint32_t lastProbe = 0;
  if (millis() - lastProbe >= probeIntervalMs()) {
    lastProbe = millis();
    sendProbe();
  }

  static uint32_t lastEmit = 0;
  if (millis() - lastEmit >= PERCEPTION_INTERVAL_MS) {
    lastEmit = millis();
    emitUartFrame();
  }

  delay(2);
}
