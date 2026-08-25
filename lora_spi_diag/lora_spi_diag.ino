// SWAP -- SX1262 raw SPI/BUSY diagnostic (v5, auto-permutation).
//
// NOT part of the bring-up test. Talks to the Core1262-HF directly over SPI
// with NO RadioLib involved.
//
// v5 change: instead of reporting one verdict and stopping, this RETRIES the
// bus under several pin/speed configurations and reports which (if any) the
// chip actually accepts commands under. If MOSI and MISO are physically
// swapped at the module header, configuration B below will succeed and you
// can fix it with no rewiring at all.
//
// Reading the result: the chip's STATUS byte is the ground truth. The SX126x
// shifts status out on MISO during almost any transaction, so:
//   - status decodes to a valid chip mode  -> MISO/SCK/NSS are working
//   - cmd=CMD_FAILED / CMD_INVALID         -> what we SEND is not arriving
//   - BUSY reacts to Calibrate             -> commands ARE arriving intact
// A configuration where BUSY reacts is a working bus.

#include <Arduino.h>
#include <SPI.h>

#define LORA_NSS   21
#define LORA_RST   33
#define LORA_BUSY  27
#define LORA_RXEN  25
#define LORA_TXEN  32

#define PIN_SCK    18
#define PIN_MISO   19
#define PIN_MOSI   23

#define OP_GET_STATUS      0xC0
#define OP_READ_REGISTER   0x1D
#define REG_VERSION_STRING 0x0320

static bool waitBusyLow(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (digitalRead(LORA_BUSY) == HIGH) {
        if (millis() - start > timeoutMs) return false;
        delay(1);
    }
    return true;
}

// Hardware reset. Returns true if BUSY dropped, i.e. the chip is powered,
// grounded and its RST/BUSY wires are intact.
static bool hardwareReset() {
    digitalWrite(LORA_RST, LOW);
    delay(2);
    digitalWrite(LORA_RST, HIGH);
    return waitBusyLow(1000);
}

static void decodeStatus(uint8_t st) {
    Serial.print("status 0x");
    if (st < 0x10) Serial.print("0");
    Serial.print(st, HEX);
    Serial.print(" (mode=");
    switch (st & 0x70) {
        case 0x20: Serial.print("STDBY_RC");   break;
        case 0x30: Serial.print("STDBY_XOSC"); break;
        case 0x40: Serial.print("FS");         break;
        case 0x50: Serial.print("RX");         break;
        case 0x60: Serial.print("TX");         break;
        default:   Serial.print("INVALID");    break;
    }
    Serial.print(", cmd=");
    switch (st & 0x0E) {
        case 0x04: Serial.print("DATA_AVAILABLE"); break;
        case 0x06: Serial.print("CMD_TIMEOUT");    break;
        case 0x08: Serial.print("CMD_INVALID");    break;
        case 0x0A: Serial.print("CMD_FAILED");     break;
        case 0x0C: Serial.print("TX_DONE");        break;
        default:   Serial.print("ok/none");        break;
    }
    Serial.print(")");
}

// Runs one full probe under the CURRENTLY configured SPI pins/speed.
// Returns true only if the chip demonstrably ACCEPTED a command, which is
// the only unambiguous proof that the outbound path (MOSI) works.
static bool probeBus(SPISettings settings, bool& sawValidMode, uint8_t& statusOut) {
    hardwareReset();

    // --- GetStatus ---
    SPI.beginTransaction(settings);
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(OP_GET_STATUS);
    statusOut = SPI.transfer(0x00);
    digitalWrite(LORA_NSS, HIGH);
    SPI.endTransaction();

    const uint8_t mode = statusOut & 0x70;
    sawValidMode = (mode >= 0x20 && mode <= 0x60);

    Serial.print("      ");
    decodeStatus(statusOut);

    // --- Version string ---
    uint8_t ver[17];
    SPI.beginTransaction(settings);
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(OP_READ_REGISTER);
    SPI.transfer((REG_VERSION_STRING >> 8) & 0xFF);
    SPI.transfer(REG_VERSION_STRING & 0xFF);
    SPI.transfer(0x00);
    for (int i = 0; i < 16; i++) ver[i] = SPI.transfer(0x00);
    digitalWrite(LORA_NSS, HIGH);
    SPI.endTransaction();
    ver[16] = '\0';

    Serial.print("  version=\"");
    for (int i = 0; i < 16; i++) {
        const uint8_t c = ver[i];
        Serial.print((c >= 32 && c < 127) ? (char)c : '.');
    }
    Serial.print("\"");

    // --- Command-reaction test: the decisive one ---
    // Calibrate makes the chip hold BUSY high for several ms. BUSY is an
    // independent return path, so this proves receipt without relying on
    // anything we read back over SPI.
    waitBusyLow(100);
    SPI.beginTransaction(settings);
    digitalWrite(LORA_NSS, LOW);
    SPI.transfer(0x89);
    SPI.transfer(0x7F);
    digitalWrite(LORA_NSS, HIGH);
    SPI.endTransaction();

    bool reacted = false;
    const uint32_t t0 = micros();
    while ((micros() - t0) < 50000UL) {
        if (digitalRead(LORA_BUSY) == HIGH) { reacted = true; break; }
    }

    Serial.println(reacted ? "  BUSY: REACTED  <== COMMANDS ARRIVING"
                           : "  BUSY: no reaction");
    waitBusyLow(200);
    return reacted;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("=== SX1262 bus diagnostic v5 (auto-permutation, no RadioLib) ===");

    pinMode(LORA_NSS, OUTPUT);  digitalWrite(LORA_NSS, HIGH);
    pinMode(LORA_RST, OUTPUT);  digitalWrite(LORA_RST, HIGH);
    pinMode(LORA_BUSY, INPUT);
    pinMode(LORA_RXEN, OUTPUT); digitalWrite(LORA_RXEN, LOW);
    pinMode(LORA_TXEN, OUTPUT); digitalWrite(LORA_TXEN, LOW);

    Serial.print("[0] Power/reset check: ");
    if (hardwareReset()) {
        Serial.println("BUSY dropped -- module powered, grounded, RST+BUSY wired OK.");
    } else {
        Serial.println("BUSY STUCK HIGH -- stop here, check 3.3V at the module and GND.");
        Serial.println("    Nothing below will be meaningful until BUSY responds.");
    }
    Serial.println();
    Serial.println("Trying bus configurations until one accepts commands:");
    Serial.println();

    bool ok = false;
    bool validMode = false;
    bool anyValidMode = false;
    uint8_t st = 0;
    int winner = 0;

    // --- A: as wired in config.h -----------------------------------------
    Serial.println("  [A] normal   SCK=18 MISO=19 MOSI=23 @ 2 MHz");
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, LORA_NSS);
    if (probeBus(SPISettings(2000000, MSBFIRST, SPI_MODE0), validMode, st)) {
        ok = true; winner = 1;
    }
    anyValidMode = anyValidMode || validMode;

    // --- B: MOSI/MISO swapped --------------------------------------------
    // If the two wires are crossed at the module header, this configuration
    // is the correct one and will simply work.
    if (!ok) {
        Serial.println("  [B] SWAPPED  SCK=18 MISO=23 MOSI=19 @ 2 MHz");
        SPI.end();
        SPI.begin(PIN_SCK, PIN_MOSI, PIN_MISO, LORA_NSS);
        if (probeBus(SPISettings(2000000, MSBFIRST, SPI_MODE0), validMode, st)) {
            ok = true; winner = 2;
        }
        anyValidMode = anyValidMode || validMode;
    }

    // --- C: normal pins, slow clock --------------------------------------
    // A marginal or long wire can pass at 500 kHz and fail at 2 MHz.
    if (!ok) {
        Serial.println("  [C] normal   SCK=18 MISO=19 MOSI=23 @ 500 kHz");
        SPI.end();
        SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, LORA_NSS);
        if (probeBus(SPISettings(500000, MSBFIRST, SPI_MODE0), validMode, st)) {
            ok = true; winner = 3;
        }
        anyValidMode = anyValidMode || validMode;
    }

    // --- D: swapped pins, slow clock -------------------------------------
    if (!ok) {
        Serial.println("  [D] SWAPPED  SCK=18 MISO=23 MOSI=19 @ 500 kHz");
        SPI.end();
        SPI.begin(PIN_SCK, PIN_MOSI, PIN_MISO, LORA_NSS);
        if (probeBus(SPISettings(500000, MSBFIRST, SPI_MODE0), validMode, st)) {
            ok = true; winner = 4;
        }
        anyValidMode = anyValidMode || validMode;
    }

    Serial.println();
    Serial.println("=== VERDICT ===");

    if (ok && (winner == 2 || winner == 4)) {
        Serial.println("WORKING -- but only with MOSI and MISO SWAPPED.");
        Serial.println("Your two wires are crossed at the module header.");
        Serial.println("Fix it EITHER way:");
        Serial.println("  - physically swap the GPIO19 and GPIO23 wires, or");
        Serial.println("  - leave the wiring and swap them in software, by calling");
        Serial.println("    SPI.begin(18, 23, 19, 21) before the radio is initialized.");
        Serial.println("Physically swapping is preferred: it keeps config.h honest and");
        Serial.println("matches the hardware reference document.");
    } else if (ok && winner == 3) {
        Serial.println("WORKING -- but only at 500 kHz, not 2 MHz.");
        Serial.println("The wiring is intact but marginal: too long, unshielded, or a");
        Serial.println("poor joint. Shorten/reseat the SPI wires. Until then the radio");
        Serial.println("will be unreliable rather than dead, which is harder to debug.");
    } else if (ok && winner == 1) {
        Serial.println("WORKING at the configured pins and speed.");
        Serial.println("The SPI bus is healthy. If the bring-up sketch still fails, the");
        Serial.println("problem is in radio configuration, not wiring.");
    } else if (anyValidMode) {
        Serial.println("NOT WORKING in any configuration -- but the chip IS answering");
        Serial.println("(status decoded to a valid chip mode), so MISO, SCK and NSS are");
        Serial.println("all fine. The outbound path is what is broken.");
        Serial.println();
        Serial.println("MOSI (GPIO23) is open. Swapping it in software did not help, so");
        Serial.println("this is not a crossed pair -- the wire itself is not conducting.");
        Serial.println("  1. Replace that jumper outright (do not just reseat it).");
        Serial.println("  2. Check the module's MOSI header joint for a cold solder.");
        Serial.println("  3. Confirm GPIO23 on the ESP32 is not damaged: unplug the");
        Serial.println("     module, jumper GPIO23 straight to GPIO19, and re-run. A");
        Serial.println("     loopback that echoes data proves the ESP32 pin is fine and");
        Serial.println("     the fault is in the wire or the module header.");
    } else {
        Serial.println("The chip is not answering at all in any configuration.");
        Serial.println("Check 3.3V at the module pin, then ground, then MISO/SCK/NSS.");
    }

    Serial.println();
    Serial.println("(Diagnostic only -- does not transmit.)");
}

void loop() {
    delay(10000);
}
