/*
  Reliable dual-SX1262 receive test for ESP32 DevKit V1 and RadioLib.

  This sketch uses two Waveshare Core1262-HF modules on one ESP32.  One module
  transmits and the other remains in continuous receive mode.  Type any text
  in the Arduino IDE Serial Monitor and press Send (line ending: Newline) to
  transmit it. The received payload, RSSI and SNR are printed immediately.

  Required library: RadioLib by Jan Gromes (Arduino Library Manager).

  Shared SPI bus:
    SCK  = GPIO18, MISO = GPIO19, MOSI = GPIO23

  Module A:
    NSS = GPIO5, RESET = GPIO14, DIO1 = GPIO26, BUSY = GPIO27
    RXEN = GPIO32, TXEN = GPIO33

  Module B:
    NSS = GPIO4, RESET = GPIO12, DIO1 = GPIO25, BUSY = GPIO13
    RXEN = GPIO16, TXEN = GPIO17

  Critical Waveshare Core1262-HF RF switch settings:
    Receive:  RXEN LOW,  TXEN HIGH
    Transmit: RXEN HIGH, TXEN LOW

  Both RXEN and TXEN must be wired for both modules.  RadioLib controls the
  SX1262 itself; this sketch controls the Waveshare board's external antenna
  switch explicitly.  It avoids the reversed and ineffective RF-switch logic
  in the previous sketch.

  Keep antennas connected before transmitting. Start with the modules at
  least 1 metre apart and TX_POWER_DBM = 10 to avoid overloading the receiver.
*/

#include <SPI.h>
#include <RadioLib.h>

// ----------------------------- Pin mapping ------------------------------
constexpr int PIN_SCK  = 18;
constexpr int PIN_MISO = 19;
constexpr int PIN_MOSI = 23;

constexpr int A_NSS  = 5;
constexpr int A_RST  = 14;
constexpr int A_DIO1 = 26;
constexpr int A_BUSY = 27;
constexpr int A_RXEN = 32;
constexpr int A_TXEN = 33;

constexpr int B_NSS  = 4;
constexpr int B_RST  = 12;
constexpr int B_DIO1 = 25;
constexpr int B_BUSY = 13;
constexpr int B_RXEN = 16;
constexpr int B_TXEN = 17;

// --------------------------- LoRa configuration -------------------------
constexpr float LORA_FREQUENCY_MHZ = 866.0F;  // IN865: valid India test band
constexpr float LORA_BANDWIDTH_KHZ = 125.0F;
constexpr uint8_t LORA_SPREADING_FACTOR = 9;
constexpr uint8_t LORA_CODING_RATE = 7;       // 4/7
constexpr uint8_t LORA_SYNC_WORD = 0x12;      // private LoRa network
constexpr uint16_t LORA_PREAMBLE_LENGTH = 8;
constexpr int8_t TX_POWER_DBM = 10;
constexpr float TCXO_VOLTAGE = 1.6F;

// Change this only if your particular RF-switch board proves to use the
// opposite levels. Unlike the previous sketch, this option really changes
// the pin levels.
constexpr bool INVERT_RF_SWITCH_LEVELS = false;

constexpr uint32_t AUTO_TRANSMIT_INTERVAL_MS = 2000;
constexpr size_t MAX_TEXT_PAYLOAD_BYTES = 240;

// ---------------------------- Radio instances ---------------------------
SX1262 radioA = new Module(A_NSS, A_DIO1, A_RST, A_BUSY, SPI);
SX1262 radioB = new Module(B_NSS, B_DIO1, B_RST, B_BUSY, SPI);

struct RadioUnit {
  SX1262& radio;
  int rxenPin;
  int txenPin;
  const char* name;
};

RadioUnit moduleA{radioA, A_RXEN, A_TXEN, "Module A"};
RadioUnit moduleB{radioB, B_RXEN, B_TXEN, "Module B"};

// Set this true only to test the opposite physical module as the receiver.
constexpr bool SWAP_MODULE_ROLES = false;
RadioUnit* transmitter = nullptr;
RadioUnit* receiver = nullptr;

// ----------------------------- Receive state -----------------------------
volatile bool packetReceived = false;
bool receiverArmed = false;
bool autoTransmit = false;
uint32_t lastAutoTransmitMs = 0;
uint32_t transmitSequence = 0;
uint32_t transmitOk = 0;
uint32_t transmitFailed = 0;
uint32_t receiveOk = 0;
uint32_t receiveCrcFailed = 0;
uint32_t receiveOtherFailed = 0;
String lastSentPayload;

void IRAM_ATTR onReceiveInterrupt() {
  // Never use Serial, String, SPI or RadioLib calls inside this ISR.
  packetReceived = true;
}

// -------------------------- RF switch functions --------------------------
// Use uint8_t constants instead of an enum class here. Arduino IDE generates
// function prototypes before this sketch body; using an enum type in a
// generated prototype can therefore produce "RfPath has not been declared".
constexpr uint8_t RF_PATH_IDLE = 0;
constexpr uint8_t RF_PATH_RECEIVE = 1;
constexpr uint8_t RF_PATH_TRANSMIT = 2;

void selectRfPath(const RadioUnit& unit, uint8_t path) {
  bool rxen = LOW;
  bool txen = LOW;

  // Waveshare Core1262-HF: RXEN LOW/TXEN HIGH selects RX; the inverse selects TX.
  if (path == RF_PATH_RECEIVE) {
    rxen = LOW;
    txen = HIGH;
  } else if (path == RF_PATH_TRANSMIT) {
    rxen = HIGH;
    txen = LOW;
  }

  if (INVERT_RF_SWITCH_LEVELS) {
    rxen = !rxen;
    txen = !txen;
  }

  digitalWrite(unit.rxenPin, rxen);
  digitalWrite(unit.txenPin, txen);
  delayMicroseconds(100);  // Allow the external RF switch to settle.
}

void prepareRfSwitch(const RadioUnit& unit) {
  pinMode(unit.rxenPin, OUTPUT);
  pinMode(unit.txenPin, OUTPUT);
  selectRfPath(unit, RF_PATH_IDLE);
}

// -------------------------- Setup and diagnostics ------------------------
bool beginRadio(RadioUnit& unit) {
  Serial.print('[');
  Serial.print(unit.name);
  Serial.print("] Initialising ... ");

  const int16_t state = unit.radio.begin(
      LORA_FREQUENCY_MHZ,
      LORA_BANDWIDTH_KHZ,
      LORA_SPREADING_FACTOR,
      LORA_CODING_RATE,
      LORA_SYNC_WORD,
      TX_POWER_DBM,
      LORA_PREAMBLE_LENGTH,
      TCXO_VOLTAGE);

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("OK");
    return true;
  }

  Serial.print("FAILED, RadioLib error ");
  Serial.println(state);
  Serial.println("  Check NSS/DIO1/RESET/BUSY wiring and 3.3 V power.");
  Serial.println("  If the error mentions TCXO, change TCXO_VOLTAGE to 0.0F.");
  return false;
}

bool armReceiver() {
  packetReceived = false;
  selectRfPath(*receiver, RF_PATH_RECEIVE);

  const int16_t state = receiver->radio.startReceive();
  receiverArmed = (state == RADIOLIB_ERR_NONE);
  if (!receiverArmed) {
    Serial.print("[RX] startReceive failed, RadioLib error ");
    Serial.println(state);
  }
  return receiverArmed;
}

void printHelp() {
  Serial.println();
  Serial.println("Serial Monitor commands (set line ending to Newline):");
  Serial.println("  Any other text  Send that text over LoRa");
  Serial.println("  /test           Send a numbered test packet");
  Serial.println("  /auto           Toggle one numbered packet every 2 seconds");
  Serial.println("  /stats          Print TX/RX counters");
  Serial.println("  /listen         Re-arm the receiver");
  Serial.println("  /help           Show this help");
  Serial.println();
}

void printStats() {
  Serial.println();
  Serial.println("---------------- LoRa statistics ----------------");
  Serial.print("Transmitted OK:       "); Serial.println(transmitOk);
  Serial.print("Transmit failures:    "); Serial.println(transmitFailed);
  Serial.print("Received OK:          "); Serial.println(receiveOk);
  Serial.print("RX CRC failures:      "); Serial.println(receiveCrcFailed);
  Serial.print("Other RX failures:    "); Serial.println(receiveOtherFailed);
  Serial.print("Receiver armed:       "); Serial.println(receiverArmed ? "yes" : "no");
  Serial.print("Active receiver:      "); Serial.println(receiver->name);
  Serial.print("Active transmitter:   "); Serial.println(transmitter->name);
  Serial.println("--------------------------------------------------");
}

// ------------------------- Transmit and receive --------------------------
void transmitText(String payload) {
  if (payload.length() == 0) {
    Serial.println("[TX] Empty payload ignored.");
    return;
  }

  if (payload.length() > MAX_TEXT_PAYLOAD_BYTES) {
    payload.remove(MAX_TEXT_PAYLOAD_BYTES);
    Serial.println("[TX] Payload truncated to 240 bytes.");
  }

  // The receiver is already armed before this point. Select TX only on the
  // transmitting module, then always restore that board to a harmless idle state.
  selectRfPath(*transmitter, RF_PATH_TRANSMIT);
  const int16_t state = transmitter->radio.transmit(payload);
  selectRfPath(*transmitter, RF_PATH_IDLE);

  if (state == RADIOLIB_ERR_NONE) {
    transmitOk++;
    lastSentPayload = payload;
    Serial.print("[TX] Sent: ");
    Serial.println(payload);
  } else {
    transmitFailed++;
    Serial.print("[TX] transmit failed, RadioLib error ");
    Serial.println(state);
  }
}

void transmitTestPacket() {
  transmitSequence++;
  transmitText("TEST #" + String(transmitSequence));
}

void handleReceivedPacket() {
  packetReceived = false;

  const size_t length = receiver->radio.getPacketLength();
  if (length == 0 || length > 255) {
    receiveOtherFailed++;
    Serial.println("[RX] Invalid packet length; re-arming receiver.");
    armReceiver();
    return;
  }

  // SX1262 LoRa packets are at most 255 bytes. Allocate one extra byte so a
  // text payload can be safely NUL-terminated for the String comparison.
  uint8_t buffer[257] = {0};
  const int16_t state = receiver->radio.readData(buffer, length);

  if (state == RADIOLIB_ERR_NONE) {
    receiveOk++;
    const float rssi = receiver->radio.getRSSI();
    const float snr = receiver->radio.getSNR();

    Serial.println();
    Serial.println("[RX] Packet received");
    Serial.print("[RX] Bytes: ");
    Serial.println(length);
    Serial.print("[RX] Text:  ");
    Serial.write(buffer, length);
    Serial.println();
    Serial.print("[RX] Hex:   ");
    for (size_t i = 0; i < length; i++) {
      if (buffer[i] < 0x10) Serial.print('0');
      Serial.print(buffer[i], HEX);
      Serial.print(i + 1 == length ? '\n' : ' ');
    }
    Serial.print("[RX] RSSI:  "); Serial.print(rssi); Serial.println(" dBm");
    Serial.print("[RX] SNR:   "); Serial.print(snr); Serial.println(" dB");

    const String receivedText(reinterpret_cast<char*>(buffer));
    if (lastSentPayload.length() > 0 && receivedText == lastSentPayload) {
      Serial.println("[RX] Payload check: exact match");
    }
  } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
    receiveCrcFailed++;
    Serial.println("[RX] CRC mismatch: packet was rejected.");
  } else {
    receiveOtherFailed++;
    Serial.print("[RX] readData failed, RadioLib error ");
    Serial.println(state);
  }

  // SX1262 exits receive after RX_DONE. Re-arm it for the next packet.
  armReceiver();
}

// --------------------------- Serial command UI ---------------------------
void processSerialLine(const String& line) {
  String command = line;
  command.trim();
  if (command.length() == 0) return;

  if (command == "/help") {
    printHelp();
  } else if (command == "/test") {
    transmitTestPacket();
  } else if (command == "/auto") {
    autoTransmit = !autoTransmit;
    lastAutoTransmitMs = millis();
    Serial.print("[TX] Auto transmit ");
    Serial.println(autoTransmit ? "enabled" : "disabled");
  } else if (command == "/stats") {
    printStats();
  } else if (command == "/listen") {
    armReceiver();
  } else {
    // Preserve spaces in ordinary application payloads.
    transmitText(line);
  }
}

void pollSerialMonitor() {
  static String line;
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      if (line.length() > 0) {
        processSerialLine(line);
        line = "";
      }
    } else if (line.length() < MAX_TEXT_PAYLOAD_BYTES) {
      line += character;
    }
  }
}

// --------------------------------- Arduino --------------------------------
void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("====================================================");
  Serial.println("Reliable SX1262 LoRa receiver test");
  Serial.println("ESP32 DevKit V1 + two Waveshare Core1262-HF modules");
  Serial.println("====================================================");

  prepareRfSwitch(moduleA);
  prepareRfSwitch(moduleB);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  if (!beginRadio(moduleA) || !beginRadio(moduleB)) {
    Serial.println("\nFix the initialisation error, then press reset.");
    while (true) delay(1000);
  }

  if (SWAP_MODULE_ROLES) {
    transmitter = &moduleB;
    receiver = &moduleA;
  } else {
    transmitter = &moduleA;
    receiver = &moduleB;
  }

  receiver->radio.setDio1Action(onReceiveInterrupt);
  if (!armReceiver()) {
    Serial.println("Receiver could not enter RX mode. Check DIO1, BUSY and RXEN/TXEN.");
  }

  Serial.print("Transmitter: "); Serial.println(transmitter->name);
  Serial.print("Receiver: "); Serial.println(receiver->name);
  Serial.println("\nReceiver is listening. Type text and press Send.");
  printHelp();
}

void loop() {
  if (packetReceived) {
    handleReceivedPacket();
  }

  if (autoTransmit && millis() - lastAutoTransmitMs >= AUTO_TRANSMIT_INTERVAL_MS) {
    lastAutoTransmitMs = millis();
    transmitTestPacket();
  }

  pollSerialMonitor();
}
