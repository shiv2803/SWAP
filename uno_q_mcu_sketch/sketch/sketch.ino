// Node A UART forwarder: reads newline-delimited JSON telemetry from Node A
// and relays each complete line to Python over the Router Bridge. Also
// receives force_protocol commands from Python and writes them back down the
// same UART to Node A.
//
// Serial2 (D14/D15), NOT Serial1 (D0/D1): usart1 is also the Zephyr console /
// boot-log UART. Sharing it with Node A meant the console wrote into the same
// line the forwarder was reading, and telemetry arrived shredded --
// {"li{"li{"li{... -- with the backend dropping nearly every frame. Serial2 is
// not claimed by the console, so the stream stays clean.
//
// Wiring (matches swap_node_a's TELEMETRY_RX_PIN/TX_PIN = GPIO16/GPIO17):
//   Node A GPIO17 (TX) -> UNO Q D14 (RX)
//   Node A GPIO16 (RX) <- UNO Q D15 (TX)
//   common GND
// 115200 8N1 on both ends.
//
// The sketch does not parse JSON itself -- it only reassembles lines and
// enforces a hard length cap, so a stuck-high line can't grow the buffer
// without bound.
#include "Arduino_RouterBridge.h"

#define NODE_A_BAUD        115200
#define LINE_BUFFER_CAP    2048

// USART2 on D14/D15.
HardwareSerial& nodeSerial = Serial2;

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
    nodeSerial.println(payload);
}

// Raw passthrough: sends an arbitrary line straight to Node A, bypassing the
// force_protocol JSON envelope. Lets you probe the physical link directly
// instead of only structured commands.
void uartSendRaw(String message) {
    nodeSerial.println(message);
}

void setup() {
    Bridge.begin();
    Bridge.provide("force_protocol", forceProtocol);
    Bridge.provide("uart_send_raw", uartSendRaw);

    nodeSerial.begin(NODE_A_BAUD);  // Serial2 @ 115200 on D14/D15

    Bridge.notify("sketch_debug_checkpoint", 4);
}

void loop() {
    while (nodeSerial.available() > 0) {
        char c = (char)nodeSerial.read();
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
