// SWAP -- GPIO integrity test for the SPI lines.
//
// Narrows down WHY MOSI (GPIO23) is not delivering commands to the SX1262.
// Three causes remain and this separates them:
//   1. the jumper wire is open
//   2. the module's MOSI header joint is cold/unsoldered
//   3. the ESP32's GPIO23 pad itself is damaged (or shorted to ground)
//
// Method: the module's MOSI pin is an INPUT on the module side, so it should
// never drive the line. That means a healthy, intact MOSI wire behaves like
// an open line from the ESP32's point of view:
//   - with the internal pull-UP enabled   -> reads HIGH
//   - with the internal pull-DOWN enabled -> reads LOW
//   - driven HIGH as an output            -> reads back HIGH
//   - driven LOW as an output             -> reads back LOW
//
// Deviations are diagnostic:
//   - pull-up reads LOW      -> the line is SHORTED TO GROUND somewhere
//   - driven HIGH reads LOW  -> the pad cannot source current: damaged GPIO,
//                               or a hard short on the net
//
// GPIO19 (MISO) is included as a CONTROL: the module actively drives it, so
// it is expected to look different from the others. That difference is what
// proves the test itself is working.
//
// Run with the LoRa module CONNECTED, exactly as it normally sits.

#include <Arduino.h>

struct PinUnderTest {
    uint8_t gpio;
    const char* name;
    const char* expectation;
};

static const PinUnderTest PINS[] = {
    { 23, "MOSI  (suspect)", "open line: pull-up HIGH, pull-down LOW, drives both ways" },
    { 18, "SCK   (control)", "open line: same as MOSI -- module input, does not drive" },
    { 19, "MISO  (control)", "module DRIVES this: may ignore pulls -- that is normal" },
    { 21, "NSS   (control)", "open line: module input, does not drive" },
};

static void testPin(const PinUnderTest& p) {
    Serial.print("GPIO");
    if (p.gpio < 10) Serial.print(" ");
    Serial.print(p.gpio);
    Serial.print("  ");
    Serial.print(p.name);
    Serial.print("  ");

    pinMode(p.gpio, INPUT_PULLUP);
    delay(5);
    const int withPullup = digitalRead(p.gpio);

    pinMode(p.gpio, INPUT_PULLDOWN);
    delay(5);
    const int withPulldown = digitalRead(p.gpio);

    pinMode(p.gpio, OUTPUT);
    digitalWrite(p.gpio, HIGH);
    delayMicroseconds(50);
    const int drivenHigh = digitalRead(p.gpio);
    digitalWrite(p.gpio, LOW);
    delayMicroseconds(50);
    const int drivenLow = digitalRead(p.gpio);

    // Leave the pin harmless afterwards.
    pinMode(p.gpio, INPUT);

    Serial.print("pull-up=");
    Serial.print(withPullup ? "HIGH" : "LOW ");
    Serial.print("  pull-down=");
    Serial.print(withPulldown ? "HIGH" : "LOW ");
    Serial.print("  drive-H=");
    Serial.print(drivenHigh ? "HIGH" : "LOW ");
    Serial.print("  drive-L=");
    Serial.print(drivenLow ? "HIGH" : "LOW ");

    // Flag the two failure signatures that matter.
    if (drivenHigh == LOW) {
        Serial.print("   <<< CANNOT DRIVE HIGH -- shorted to GND or dead pad");
    } else if (withPullup == LOW && withPulldown == LOW) {
        Serial.print("   <<< HELD LOW -- something is pulling this line to GND");
    } else if (withPullup == HIGH && withPulldown == HIGH) {
        Serial.print("   <<< HELD HIGH -- shorted to 3.3V");
    }
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println();
    Serial.println("=== GPIO integrity test (LoRa module should be CONNECTED) ===");
    Serial.println();

    for (const PinUnderTest& p : PINS) {
        testPin(p);
    }

    Serial.println();
    Serial.println("=== HOW TO READ THIS ===");
    Serial.println("GPIO23 should look like GPIO18 and GPIO21 (all module inputs).");
    Serial.println();
    Serial.println("If GPIO23 matches them (pull-up HIGH, pull-down LOW, drives both):");
    Serial.println("  The pad is HEALTHY and the net is not shorted. The fault is then");
    Serial.println("  a broken connection between the ESP32 pin and the module's MOSI");
    Serial.println("  pad -- an open jumper or a cold solder joint at the header.");
    Serial.println("  Replace the jumper; if that fails, reflow the module's MOSI pin.");
    Serial.println();
    Serial.println("If GPIO23 shows CANNOT DRIVE HIGH or HELD LOW:");
    Serial.println("  The pad is damaged or the net is shorted to ground. Do not keep");
    Serial.println("  fighting it -- remap MOSI to a free pin instead. GPIO4 and GPIO13");
    Serial.println("  are both free now that the OLED is gone, and ESP32 SPI can use");
    Serial.println("  any GPIO. Move the wire, then initialise SPI explicitly before");
    Serial.println("  the radio, e.g.:  SPI.begin(18, 19, 4, 21);");
    Serial.println();
    Serial.println("If GPIO18/21 ALSO show warnings, the test is being confused by the");
    Serial.println("module -- unplug it and re-run to test the ESP32 pads in isolation.");
}

void loop() {
    delay(10000);
}
