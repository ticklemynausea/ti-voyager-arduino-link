/*
 * esp32_tilink_linetest.ino - raw line-state monitor for the TI link wiring
 *
 * Never drives the lines; only reads them. Prints the state of tip and ring
 * whenever either changes, plus a heartbeat every 2 s.
 * Serial Monitor: 115200 baud.
 *
 * Expected with the calculator on, at the Home screen:
 *   idle:                     tip=1 ring=1
 *   short red   to black:     tip=0 ring=1
 *   short white to black:     tip=1 ring=0
 * A line that flickers when you wiggle its wire has a bad contact.
 */

// Classic ESP32: 25 / 26.  M5Stack Stamp S3: 1 / 5.
const uint8_t PIN_TIP  = 1;   // red   (via 220 ohm)
const uint8_t PIN_RING = 5;   // white (via 220 ohm)

int lastTip = -1, lastRing = -1;
uint32_t changes = 0, lastBeat = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TIP, INPUT);
  pinMode(PIN_RING, INPUT);
  delay(500);
  Serial.println("\nTI link line test. Calculator on, Home screen.");
}

void loop() {
  int tip = digitalRead(PIN_TIP);
  int ring = digitalRead(PIN_RING);

  if (tip != lastTip || ring != lastRing) {
    changes++;
    Serial.printf("%8lu ms  tip=%d ring=%d%s\n", (unsigned long)millis(), tip, ring,
                  (tip && ring) ? "  (idle)" : "");
    lastTip = tip;
    lastRing = ring;
  }

  if (millis() - lastBeat > 2000) {
    lastBeat = millis();
    Serial.printf("  ... tip=%d ring=%d  changes so far: %lu\n", tip, ring, (unsigned long)changes);
  }
}
