#include "link_manager.h"
#include <ArduinoJson.h>
#include <string.h>

// ---- BLE exchange state ----------------------------------------------------
// File-scope, not member variables: NimBLE-Arduino callback classes don't
// carry a LinkManager* back-pointer here, and this firmware only ever runs
// one LinkManager instance (see swap_node.ino), so a singleton-style static
// is simplest. Reused on both roles as "the PING/PONG round I'm timing just
// completed" — semantics documented at each use site below.
namespace {
volatile bool s_bleExchangeSignal = false;
// Holds whatever Node B just wrote to bleChar_ -- its own telemetry CSV now,
// not a bare "PONG". Set in BleCharCallbacks::onWrite(), read back in
// tryBleExchange() right after s_bleExchangeSignal wakes it.
String s_bleExchangePayload;
constexpr uint32_t BLE_SCAN_SECONDS = 5;

#ifdef NODE_ROLE_A
// Node A = BLE peripheral/server. Central writes back to bleChar_ once it
// has processed our request notification; that write now carries Node B's
// real telemetry, not just an ack.
class BleServerCallbacks : public NimBLEServerCallbacks {
    // NimBLE-Arduino 2.x signatures (verified against the installed 2.3.1
    // headers) — onConnect gained NimBLEConnInfo&, onDisconnect gained both
    // NimBLEConnInfo& and an int reason code, vs. the 1.4.x signatures this
    // was originally written against.
    void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*connInfo*/) override {
        Serial.println("[BLE] central connected");
    }
    void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
        Serial.println("[BLE] central disconnected");
        NimBLEDevice::startAdvertising();
    }
};

class BleCharCallbacks : public NimBLECharacteristicCallbacks {
    // NimBLEAttValue::c_str()/length() verified against the installed
    // NimBLE-Arduino 2.5.1 headers (NimBLEAttValue.h) -- safe here because
    // the payload is plain ASCII CSV, never raw/embedded-null binary.
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& /*connInfo*/) override {
        NimBLEAttValue val = characteristic->getValue();
        s_bleExchangePayload = String(val.c_str());
        s_bleExchangeSignal = true;
    }
};

BleServerCallbacks g_bleServerCallbacks;
BleCharCallbacks g_bleCharCallbacks;
#else
// Node B = BLE central/client. A's request arrives as a notification; we
// reply by writing our own current telemetry CSV (built from the global
// `linkManager` instance defined in the .ino), replacing the old bare
// "PONG" ack.
extern LinkManager linkManager;

void onBleNotify(NimBLERemoteCharacteristic* remoteChar, uint8_t* /*data*/, size_t /*len*/, bool /*isNotify*/) {
    s_bleExchangeSignal = true;
    if (remoteChar->canWrite()) {
        String payload = linkManager.buildOwnPeerPayload();
        remoteChar->writeValue(payload.c_str(), false);
    }
}
#endif
}  // namespace

void LinkManager::begin() {
    TELEMETRY_SERIAL.begin(TELEMETRY_BAUD, SERIAL_8N1, TELEMETRY_RX_PIN, TELEMETRY_TX_PIN);

    setupLoRa();
    beginWifi();
    beginBle();
}

void LinkManager::beginWifi() {
#ifdef NODE_ROLE_A
    // Node A hosts the SoftAP; Node B connects as a station.
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    wifiTcpServer_.begin();
    Serial.print("[WiFi] SoftAP up, IP=");
    Serial.println(WiFi.softAPIP());
#else
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WiFi] STA connected, IP=");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WiFi] STA connect timed out — will retry lazily during switching");
    }
#endif
}

void LinkManager::beginBle() {
    NimBLEDevice::init(BLE_DEVICE_NAME);

#ifdef NODE_ROLE_A
    bleServer_ = NimBLEDevice::createServer();
    bleServer_->setCallbacks(&g_bleServerCallbacks);
    NimBLEService* svc = bleServer_->createService(BLE_SERVICE_UUID);
    bleChar_ = svc->createCharacteristic(
        BLE_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE);
    bleChar_->setCallbacks(&g_bleCharCallbacks);
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->start();
    Serial.println("[BLE] advertising as peripheral (Node A)");
#else
    Serial.println("[BLE] scanning for Node A...");
    // NimBLE-Arduino 2.x: scan->start() now returns bool (did the scan start
    // OK), not the results themselves; getResults(duration, isContinue) is
    // the direct replacement for the old "start and block until done,
    // returning results" pattern. getDevice() also returns a pointer now,
    // not a value -- verified against the installed 2.5.1 headers.
    NimBLEScan* scan = NimBLEDevice::getScan();
    NimBLEScanResults results = scan->getResults(BLE_SCAN_SECONDS, false);

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice* device = results.getDevice(i);
        if (device->isAdvertisingService(NimBLEUUID(BLE_SERVICE_UUID))) {
            bleClient_ = NimBLEDevice::createClient();
            if (bleClient_->connect(device)) {
                NimBLERemoteService* svc = bleClient_->getService(BLE_SERVICE_UUID);
                if (svc != nullptr) {
                    bleRemoteChar_ = svc->getCharacteristic(BLE_CHAR_UUID);
                    if (bleRemoteChar_ != nullptr && bleRemoteChar_->canNotify()) {
                        bleRemoteChar_->subscribe(true, onBleNotify);
                        Serial.println("[BLE] connected + subscribed to Node A (central role)");
                    }
                }
            }
            break;
        }
    }
    if (bleClient_ == nullptr || !bleClient_->isConnected()) {
        Serial.println("[BLE] Node A not found this scan — will retry lazily during switching");
    }
#endif
}

bool LinkManager::tryWifiExchange() {
    LinkQualitySample sample;
    sample.rssi_dbm = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -999.0f;

    bool ok = false;
#ifdef NODE_ROLE_A
    if (wifiTcpServer_.hasClient() && !wifiClient_.connected()) {
        wifiClient_ = wifiTcpServer_.available();
    }
    if (wifiClient_.connected()) {
        // Ask Node B for its telemetry instead of a bare PING -- its reply
        // (a newline-terminated CSV line, not 4 fixed bytes) is what we're
        // actually after now.
        const uint32_t t0 = millis();
        wifiClient_.write("REQB\n", 5);
        String line;
        bool gotLine = false;
        const uint32_t start = millis();
        while (millis() - start < LINK_TIMEOUT_MS && !gotLine) {
            while (wifiClient_.available() > 0) {
                const char c = (char)wifiClient_.read();
                if (c == '\n') { gotLine = true; break; }
                if (c != '\r') line += c;
            }
            if (!gotLine) delay(2);
        }
        if (gotLine) {
            parsePeerPayload(line);
            sample.rtt_ms = millis() - t0;
            ok = true;
        }
    }
#else
    if (!wifiClient_.connected()) {
        wifiClient_.connect(WiFi.gatewayIP(), WIFI_PORT);
    }
    if (wifiClient_.connected() && wifiClient_.available() > 0) {
        // Whatever Node A sent (the "REQB" trigger) -- its arrival is the
        // cue to reply with our own telemetry, content doesn't matter.
        while (wifiClient_.available() > 0) wifiClient_.read();
        String payload = buildOwnPeerPayload();
        payload += '\n';
        wifiClient_.print(payload);
        ok = true;
    }
#endif
    sample.packet_ok = ok;
    wifiMetrics_.push(sample);
    return ok;
}

bool LinkManager::tryBleExchange() {
    LinkQualitySample sample;

#ifdef NODE_ROLE_A
    const bool connected = bleServer_ != nullptr && bleServer_->getConnectedCount() > 0;
    if (connected) {
        s_bleExchangeSignal = false;
        s_bleExchangePayload = "";
        const uint32_t t0 = millis();
        bleChar_->setValue("REQB");
        bleChar_->notify();

        while (!s_bleExchangeSignal && (millis() - t0) < LINK_TIMEOUT_MS) {
            delay(2);
        }
        if (s_bleExchangeSignal) {
            if (s_bleExchangePayload.length() > 0) {
                parsePeerPayload(s_bleExchangePayload);
            }
            sample.rtt_ms = millis() - t0;
            sample.packet_ok = true;
        }
    }
    // NOTE: NimBLE-Arduino's NimBLEServer API doesn't expose a simple
    // per-connection RSSI getter the way NimBLEClient::getRssi() does on the
    // central side below — Node A's ble_rssi telemetry stays at the -999
    // sentinel deliberately, not by oversight. A real reading needs a
    // lower-level ble_gap_conn_rssi(conn_handle, &rssi) call into the
    // underlying NimBLE host stack.
#else
    if (bleClient_ == nullptr || !bleClient_->isConnected()) {
        beginBle();  // lazy reconnect attempt
    }
    if (bleClient_ != nullptr && bleClient_->isConnected()) {
        sample.rssi_dbm = static_cast<float>(bleClient_->getRssi());

        s_bleExchangeSignal = false;
        const uint32_t start = millis();
        while (!s_bleExchangeSignal && (millis() - start) < LINK_TIMEOUT_MS) {
            delay(2);
        }
        if (s_bleExchangeSignal) {
            sample.rtt_ms = millis() - start;
            sample.packet_ok = true;
        }
    }
#endif

    bleMetrics_.push(sample);
    return sample.packet_ok;
}

bool LinkManager::tryLoraExchange() {
#ifdef NODE_ROLE_A
    // Node A just beacons -- it doesn't need to send its own numbers over
    // the air, it already has them locally for its own UART frame.
    const bool txOk = sendLoRaTelemetry("SWAP_PING");
#else
    // Node B transmits its own telemetry CSV instead of a bare ping --
    // sendLoRaTelemetry() takes a const char*, safe here since CSV text has
    // no embedded nulls (unlike a raw binary struct would).
    String loraPayload = buildOwnPeerPayload();
    const bool txOk = sendLoRaTelemetry(loraPayload.c_str());
#endif

    uint8_t rxBuf[96];
    size_t rxLen = 0;
    const bool rxOk = receiveLoRaTelemetry(rxBuf, sizeof(rxBuf), rxLen, LINK_TIMEOUT_MS);

#ifdef NODE_ROLE_A
    if (rxOk && rxLen > 0 && rxLen < sizeof(rxBuf)) {
        rxBuf[rxLen] = '\0';
        String received(reinterpret_cast<char*>(rxBuf));
        if (received != "SWAP_PING") {
            parsePeerPayload(received);
        }
    }
#endif

    LinkQualitySample sample;
    const LoraMetrics& m = getLoraMetrics();
    sample.rssi_dbm = m.rssi_dbm;
    sample.snr_db = m.snr_db;
    sample.rtt_ms = m.last_rtt_ms;
    sample.packet_ok = txOk && rxOk;
    loraMetrics_.push(sample);
    return sample.packet_ok;
}

void LinkManager::evaluateAndSwitch() {
    ActiveProtocol next = active_;

    if (forcedProtocol_ >= 0) {
        next = static_cast<ActiveProtocol>(forcedProtocol_);
    } else {
        const bool wifiHealthy = (wifiMetrics_.avgRssi() > WIFI_RSSI_MIN_DBM)
                               && (wifiMetrics_.packetLossRate() < WIFI_PACKET_LOSS_MAX)
                               && wifiMetrics_.sampleCount() >= 3;
        const bool bleHealthy = (bleMetrics_.avgRssi() > BLE_RSSI_MIN_DBM) && bleMetrics_.sampleCount() >= 3;

        switch (active_) {
            case ActiveProtocol::WIFI:
                if (!wifiHealthy) next = bleHealthy ? ActiveProtocol::BLE : ActiveProtocol::LORA;
                break;
            case ActiveProtocol::BLE:
                if (wifiHealthy) next = ActiveProtocol::WIFI;
                else if (!bleHealthy) next = ActiveProtocol::LORA;
                break;
            case ActiveProtocol::LORA:
                if (wifiHealthy) next = ActiveProtocol::WIFI;
                else if (bleHealthy) next = ActiveProtocol::BLE;
                break;
        }
    }

    if (next != active_) {
        Serial.print("[LinkManager] switching protocol: ");
        Serial.print(static_cast<int>(active_));
        Serial.print(" -> ");
        Serial.println(static_cast<int>(next));
        active_ = next;
    }
}

void LinkManager::pollIncomingCommands() {
    // Newline-delimited JSON from swap_backend's SerialCommandSender, e.g.
    // {"cmd":"force_protocol","protocol":2}\n — see telemetry_link.py.
    while (TELEMETRY_SERIAL.available()) {
        const char c = static_cast<char>(TELEMETRY_SERIAL.read());
        if (c == '\n' || c == '\r') {
            if (commandLineBuf_.length() > 0) {
                StaticJsonDocument<128> doc;
                if (deserializeJson(doc, commandLineBuf_) == DeserializationError::Ok) {
                    const char* cmd = doc["cmd"] | "";
                    if (strcmp(cmd, "force_protocol") == 0) {
                        const int protocol = doc["protocol"] | -1;
                        if (protocol >= 0 && protocol <= 2) {
                            forcedProtocol_ = protocol;
                            Serial.print("[LinkManager] forced protocol -> ");
                            Serial.println(protocol);
                        }
                    }
                }
                commandLineBuf_ = "";
            }
        } else if (commandLineBuf_.length() < 120) {
            commandLineBuf_ += c;
        }
    }
}

void LinkManager::emitTelemetryFrame() {
    StaticJsonDocument<256> doc;
#ifdef NODE_ROLE_A
    doc["node"] = "a";
#else
    doc["node"] = "b";
#endif
    doc["ts_ms"] = millis();
    doc["active_protocol"] = static_cast<int>(active_);
    doc["wifi_rssi"] = wifiMetrics_.avgRssi();
    doc["wifi_loss"] = wifiMetrics_.packetLossRate();
    doc["ble_rssi"] = bleMetrics_.avgRssi();
    doc["lora_rssi"] = loraMetrics_.avgRssi();
    doc["lora_snr"] = getLoraMetrics().snr_db;
    doc["lora_loss"] = loraMetrics_.packetLossRate();

    // rtt_ms reported against whichever protocol is currently active — that's
    // the RTT actually driving the live decision, and train_model.py requires
    // this column to be present when training on real (non-simulated) logs.
    switch (active_) {
        case ActiveProtocol::WIFI: doc["rtt_ms"] = wifiMetrics_.avgRtt(); break;
        case ActiveProtocol::BLE:  doc["rtt_ms"] = bleMetrics_.avgRtt();  break;
        case ActiveProtocol::LORA: doc["rtt_ms"] = loraMetrics_.avgRtt(); break;
    }

    serializeJson(doc, TELEMETRY_SERIAL);
    TELEMETRY_SERIAL.println();
}

String LinkManager::buildOwnPeerPayload() const {
    float rtt;
    switch (active_) {
        case ActiveProtocol::WIFI: rtt = wifiMetrics_.avgRtt(); break;
        case ActiveProtocol::BLE:  rtt = bleMetrics_.avgRtt();  break;
        case ActiveProtocol::LORA: rtt = loraMetrics_.avgRtt(); break;
        default: rtt = 0.0f; break;
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "%.1f,%.3f,%.1f,%.1f,%.1f,%.3f,%.0f",
             wifiMetrics_.avgRssi(), wifiMetrics_.packetLossRate(),
             bleMetrics_.avgRssi(), loraMetrics_.avgRssi(),
             getLoraMetrics().snr_db, loraMetrics_.packetLossRate(), rtt);
    return String(buf);
}

void LinkManager::parsePeerPayload(const String& csv) {
    float vals[7];
    int idx = 0;
    int start = 0;
    for (int i = 0; i <= static_cast<int>(csv.length()) && idx < 7; i++) {
        if (i == static_cast<int>(csv.length()) || csv[i] == ',') {
            vals[idx++] = csv.substring(start, i).toFloat();
            start = i + 1;
        }
    }
    if (idx != 7) return;  // malformed line -- ignore rather than relay garbage
    nodeBWifiRssi_ = vals[0];
    nodeBWifiLoss_ = vals[1];
    nodeBBleRssi_  = vals[2];
    nodeBLoraRssi_ = vals[3];
    nodeBLoraSnr_  = vals[4];
    nodeBLoraLoss_ = vals[5];
    nodeBRttMs_    = vals[6];
    haveNodeBTelemetry_ = true;
}

void LinkManager::emitNodeBTelemetryFrame() {
    if (!haveNodeBTelemetry_) return;  // never heard from Node B yet
    StaticJsonDocument<256> doc;
    doc["node"] = "b";
    doc["ts_ms"] = millis();
    doc["active_protocol"] = static_cast<int>(active_);
    doc["wifi_rssi"] = nodeBWifiRssi_;
    doc["wifi_loss"] = nodeBWifiLoss_;
    doc["ble_rssi"] = nodeBBleRssi_;
    doc["lora_rssi"] = nodeBLoraRssi_;
    doc["lora_snr"] = nodeBLoraSnr_;
    doc["lora_loss"] = nodeBLoraLoss_;
    doc["rtt_ms"] = nodeBRttMs_;
    serializeJson(doc, TELEMETRY_SERIAL);
    TELEMETRY_SERIAL.println();
}

void LinkManager::loop() {
    pollIncomingCommands();

    switch (active_) {
        case ActiveProtocol::WIFI: tryWifiExchange(); break;
        case ActiveProtocol::BLE:  tryBleExchange();  break;
        case ActiveProtocol::LORA: tryLoraExchange(); break;
    }

    // Background-sample the inactive protocols at a lower rate so the
    // switching decision has fresh data without saturating the radios.
    static uint32_t lastBackgroundSample = 0;
    if (millis() - lastBackgroundSample > 2000) {
        if (active_ != ActiveProtocol::WIFI) tryWifiExchange();
        if (active_ != ActiveProtocol::BLE) tryBleExchange();
        if (active_ != ActiveProtocol::LORA) tryLoraExchange();
        lastBackgroundSample = millis();
    }

    evaluateAndSwitch();

    static uint32_t lastTelemetry = 0;
    if (millis() - lastTelemetry > TELEMETRY_INTERVAL_MS) {
        emitTelemetryFrame();
#ifdef NODE_ROLE_A
        emitNodeBTelemetryFrame();
#endif
        lastTelemetry = millis();
    }
}
