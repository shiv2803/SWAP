#include "config.h"
#include "link_manager.h"

LinkManager linkManager;

void setup() {
    Serial.begin(115200);
    delay(300);
#ifdef NODE_ROLE_A
    Serial.println("=== SWAP Node A booting ===");
#else
    Serial.println("=== SWAP Node B booting ===");
#endif
    linkManager.begin();
}

void loop() {
    linkManager.loop();
}
