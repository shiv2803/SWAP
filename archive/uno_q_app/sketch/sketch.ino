// Node A UART forwarder: reads newline-delimited JSON telemetry from Node A
// on Serial1 (D0=RX/PB7, D1=TX/PB6, 115200 8N1) and relays each complete
// line to Python over the Router Bridge. Also receives force_protocol
// commands from Python and writes them back down the same UART to Node A.
//
// Using D0/D1 (usart1) instead of D20/D21 (usart3) because Node A is already
// wired there. Running at 115200 (not 9600) because usart1 is also the
// Zephyr console/boot-log UART, which appears to hold the line at its own
// 115200 default regardless of what Serial1.begin() requests -- 9600
// produced a baud mismatch (near-zero valid frames received). Matching
// 115200 on both ends avoids fighting the console for ownership of the rate.
//
// The sketch does not parse JSON itself -- it only reassembles lines and
// enforces a hard length cap, so a stuck-high line can't grow the buffer
// without bound.
#include "Arduino_RouterBridge.h"

#define NODE_A_BAUD 115200
#define LINE_BUFFER_CAP 2048

char lineBuffer[LINE_BUFFER_CAP];
size_t lineLength = 0;
bool lineOverflowed = false;

unsigned long framesForwarded = 0;
unsigned long framesOverflowed = 0;
unsigned long bytesSeen = 0;
unsigned long lastHeartbeatMillis = 0;
const unsigned long heartbeatIntervalMs = 2000;

void forceProtocol(String node, int protocol) {
    String payload = "{\"cmd\":\"force_protocol\",\"node\":\"" + node + "\",\"protocol\":" + String(protocol) + "}";
    Serial1.println(payload);
}

// Raw passthrough (from UART_Demo): sends an arbitrary line straight to
// Node A on D0/D1, bypassing the force_protocol JSON envelope. Lets you
// probe the physical link directly instead of only structured commands.
void uartSendRaw(String message) {
    Serial1.println(message);
}

void setup() {
    Bridge.begin();
    Bridge.provide("force_protocol", forceProtocol);
    Bridge.provide("uart_send_raw", uartSendRaw);
    Serial1.begin(NODE_A_BAUD);
    Bridge.notify("sketch_debug_checkpoint", 4);
}

void loop() {
    while (Serial1.available() > 0) {
        char c = (char)Serial1.read();
        bytesSeen++;

        if (c == '\n') {
            if (lineLength > 0 && !lineOverflowed) {
                lineBuffer[lineLength] = '\0';
                Bridge.notify("telemetry_line", String(lineBuffer));
                framesForwarded++;
            } else if (lineOverflowed) {
                framesOverflowed++;
            }
            lineLength = 0;
            lineOverflowed = false;
            continue;
        }

        if (c == '\r') {
            continue; // tolerate CRLF
        }

        if (lineLength < (LINE_BUFFER_CAP - 1)) {
            lineBuffer[lineLength++] = c;
        } else {
            // Hard cap hit: mark this line as overflowed and keep draining
            // bytes until the next '\n' without growing the buffer further.
            lineOverflowed = true;
        }
    }

    unsigned long now = millis();
    if (now - lastHeartbeatMillis >= heartbeatIntervalMs) {
        lastHeartbeatMillis = now;
        Bridge.notify("sketch_debug_hb", bytesSeen);
    }
}
