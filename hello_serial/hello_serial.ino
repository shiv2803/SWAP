// Minimal serial sanity check. No SPI, no radio, no pins touched.
//
// Purpose: prove that upload + board + Serial Monitor all work, before
// blaming any diagnostic sketch. If THIS prints nothing, the problem is the
// toolchain/board/port, not the LoRa code -- and no amount of editing the
// diagnostic will help.
//
// Expected: a banner, then a counter line once per second, forever.

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println();
    Serial.println("=== HELLO: serial is working ===");
    Serial.print("Chip cores: ");
    Serial.println(ESP.getChipCores());
    Serial.print("Free heap: ");
    Serial.println(ESP.getFreeHeap());
    Serial.println("If you can read this, upload and Serial Monitor are fine.");
}

void loop() {
    static uint32_t n = 0;
    Serial.print("tick ");
    Serial.println(n++);
    delay(1000);
}
