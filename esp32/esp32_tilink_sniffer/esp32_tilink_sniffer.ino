/*
 * esp32_tilink_sniffer.ino - receive-only TI link sniffer
 *
 * Accepts every byte the calculator sends (doing the bit handshake) and
 * prints it in hex to the Serial Monitor (115200 baud). Bytes arriving
 * close together are grouped into one line; a pause starts a new line.
 * The first 4 bytes of each burst are decoded as a TI packet header:
 *   machine ID | command | length (2 bytes, little-endian)
 *
 * The ESP32 does NOT answer with TI packets, so after its first packet the
 * calculator waits for a reply and eventually shows a link error (or press
 * ON to break). That's expected - we only want to see what it sends.
 *
 * Try on the calculator (Home screen or a BASIC program):
 *   Send {1,2,3}      CBL-style send
 *   Get x             CBL-style request (press ON to abort)
 *   SendCalc x        calc-to-calc send (store something in x first)
 *
 * Needs TiLink.h / TiLink.cpp in the same folder.
 */
#include "TiLink.h"

// Classic ESP32: 25 / 26.  M5Stack Stamp S3: 1 / 2.
const uint8_t PIN_TIP  = 1;
const uint8_t PIN_RING = 2;

const uint32_t BURST_GAP_MS = 30;  // pause that ends a burst

TiLink ti(PIN_TIP, PIN_RING);

uint8_t  burst[300];
uint16_t burstLen = 0;
uint32_t lastByteAt = 0;
uint32_t burstCount = 0;

const char *cmdName(uint8_t c) {
  switch (c) {
    case 0x06: return "VAR (variable header)";
    case 0x09: return "CTS (clear to send)";
    case 0x15: return "DATA";
    case 0x36: return "SKIP/EXIT";
    case 0x56: return "ACK";
    case 0x5A: return "ERR (checksum error)";
    case 0x68: return "RDY (ready check)";
    case 0x6D: return "SCR (screenshot request)";
    case 0x78: return "CONT";
    case 0x87: return "CMD (remote command)";
    case 0x92: return "EOT (end of transmission)";
    case 0xA2: return "REQ (request variable)";
    case 0xC9: return "RTS (request to send)";
    default:   return "?";
  }
}

void flushBurst() {
  if (burstLen == 0) return;
  burstCount++;
  Serial.printf("\n#%lu  %u bytes\n", (unsigned long)burstCount, burstLen);

  if (burstLen >= 4) {
    uint16_t len = burst[2] | (burst[3] << 8);
    Serial.printf("  machine id 0x%02X  command 0x%02X %s  length %u\n",
                  burst[0], burst[1], cmdName(burst[1]), len);
  }

  Serial.print("  hex:  ");
  for (uint16_t i = 0; i < burstLen; i++) {
    Serial.printf("%02X ", burst[i]);
    if (i % 16 == 15 && i + 1 < burstLen) Serial.print("\n        ");
  }
  Serial.print("\n  text: ");
  for (uint16_t i = 0; i < burstLen; i++) {
    char c = (char)burst[i];
    Serial.print((c >= 32 && c < 127) ? c : '.');
  }
  Serial.println();
  burstLen = 0;
}

void setup() {
  Serial.begin(115200);
  ti.begin();
  delay(500);
  Serial.println("\nTI link sniffer ready.");
  Serial.printf("Line idle: %s\n", ti.lineIdle() ? "yes" : "NO - check wiring / calculator on?");
  Serial.println("Now run Send {1,2,3} or SendCalc on the calculator.");
}

void loop() {
  int b = ti.recvByte(0);
  if (b >= 0) {
    if (burstLen < sizeof(burst)) burst[burstLen++] = (uint8_t)b;
    lastByteAt = millis();
  } else if (b == -2) {
    Serial.println("\n! byte error (handshake broke mid-byte)");
  }

  if (burstLen > 0 && millis() - lastByteAt > BURST_GAP_MS) flushBurst();
}
