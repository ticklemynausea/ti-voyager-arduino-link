/*
 * esp32_tilink_pingpong.ino - ping-pong demo between an ESP32 and a Voyage 200
 * Pairs with pingpong.c running on the calculator.
 *
 * Messages (text frames):
 *   "PING <n>"  -> the other side answers "PONG <n>"
 *
 * On the ESP32:
 *   BOOT button, or 'p' in the Serial Monitor  -> send a ping to the calculator
 *   's' -> print statistics     'r' -> reset statistics
 * Serial Monitor: 115200 baud.
 *
 * Avoid pinging from the ESP32 while the calculator is in auto-ping mode:
 * if both sides start a byte at the same instant the frame is lost (it
 * recovers on its own, but it shows up as an error).
 */
#include "TiLink.h"

const uint8_t  PIN_TIP    = 1;    // Stamp S3 G1 -> 220R -> jack tip  (red)   [classic ESP32: 25]
const uint8_t  PIN_RING   = 5;    // Stamp S3 G5 -> 220R -> jack ring (white) [classic ESP32: 26]
const uint8_t  PIN_BUTTON = 0;    // Stamp S3 button / BOOT button on ESP32 dev boards
const uint32_t PONG_TIMEOUT_MS = 1000;

TiLink ti(PIN_TIP, PIN_RING);

uint8_t  rx[TiLink::MAX_PAYLOAD + 1];
uint32_t pingSeq = 0, pingSentAt = 0;
bool     awaitingPong = false;
bool     lastButton = HIGH;

// stats
uint32_t pingsSent = 0, pongsOk = 0, pingsLost = 0, calcPingsAnswered = 0, badFrames = 0;

void sendPing() {
  char msg[24];
  snprintf(msg, sizeof(msg), "PING %lu", (unsigned long)++pingSeq);
  if (ti.sendText(msg)) {
    pingsSent++;
    awaitingPong = true;
    pingSentAt = millis();
    Serial.printf("> %s\n", msg);
  } else {
    Serial.println("! ping failed - is pingpong running on the calculator?");
  }
}

void printStats() {
  Serial.println("---- stats ----");
  Serial.printf("ESP32 pings sent: %lu  pongs ok: %lu  lost: %lu\n",
                (unsigned long)pingsSent, (unsigned long)pongsOk, (unsigned long)pingsLost);
  Serial.printf("calc pings answered: %lu\n", (unsigned long)calcPingsAnswered);
  Serial.printf("frames sent: %lu  received: %lu  bad frames: %lu  link errors: %lu\n",
                (unsigned long)ti.framesSent, (unsigned long)ti.framesRecv,
                (unsigned long)badFrames, (unsigned long)ti.errors);
}

void handleFrame(const char *msg) {
  if (strncmp(msg, "PING ", 5) == 0) {
    // answer first, print afterwards, so the calculator isn't kept waiting
    char reply[TiLink::MAX_PAYLOAD + 1];
    snprintf(reply, sizeof(reply), "PONG %s", msg + 5);
    bool ok = ti.sendText(reply);
    if (ok) calcPingsAnswered++;
    Serial.printf("< %s  -> %s\n", msg, ok ? reply : "(reply failed)");

  } else if (strncmp(msg, "PONG ", 5) == 0) {
    unsigned long n = strtoul(msg + 5, nullptr, 10);
    if (awaitingPong && n == pingSeq) {
      awaitingPong = false;
      pongsOk++;
      Serial.printf("< %s  rtt=%lu ms\n", msg, (unsigned long)(millis() - pingSentAt));
    } else {
      Serial.printf("< stray %s\n", msg);
    }

  } else {
    Serial.printf("< %s\n", msg);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  ti.begin();
  delay(200);
  Serial.println("\nTI link ping-pong ready.");
  Serial.println("BOOT button or 'p' = ping, 's' = stats, 'r' = reset stats");
  Serial.printf("Line idle: %s\n", ti.lineIdle() ? "yes" : "NO - check wiring / calc");
}

void loop() {
  // 1. incoming frames from the calculator
  int n = ti.pollFrame(rx);
  if (n > 0) {
    handleFrame((const char *)rx);
  } else if (n < 0) {
    badFrames++;
    Serial.println("! bad frame");
  }

  // 2. pong timeout
  if (awaitingPong && millis() - pingSentAt > PONG_TIMEOUT_MS) {
    awaitingPong = false;
    pingsLost++;
    Serial.printf("! PING %lu timed out\n", (unsigned long)pingSeq);
  }

  // 3. BOOT button (falling edge, crude debounce)
  bool button = digitalRead(PIN_BUTTON);
  if (lastButton == HIGH && button == LOW) {
    delay(20);
    if (digitalRead(PIN_BUTTON) == LOW) sendPing();
  }
  lastButton = button;

  // 4. Serial Monitor commands
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'p') sendPing();
    else if (c == 's') printStats();
    else if (c == 'r') {
      pingsSent = pongsOk = pingsLost = calcPingsAnswered = badFrames = 0;
      ti.framesSent = ti.framesRecv = ti.errors = 0;
      Serial.println("stats reset");
    }
  }
}
