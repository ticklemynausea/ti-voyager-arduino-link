/*
 * ti_link_arduino.ino
 * Raw-byte link between an Arduino and a TI-89 / TI-92+ / Voyager 200.
 * Pairs with tilink.c running on the calculator.
 *
 * Wiring (2.5 mm TRS plug):
 *   Tip    (red)    -> D2
 *   Ring   (white)  -> D3
 *   Sleeve (copper) -> GND
 * 5 V boards: direct (optionally 100-220 ohm series resistors).
 * ESP32 / 3.3 V boards: use a BSS138-style bidirectional level shifter on both lines.
 *
 * Frame format (both directions):
 *   0x7E | len (1..120) | payload[len] | checksum (sum of payload bytes, mod 256)
 *
 * Demo behaviour:
 *   - Frames from the calculator are printed to Serial and answered:
 *       "HELLO" -> "Hi from Arduino"
 *       "A0?"   -> "A0=<analogRead(A0)>"
 *       other   -> "echo:<text>"
 *   - Any line typed in the Serial Monitor (115200 baud, newline ending)
 *     is sent to the calculator.
 */

const uint8_t PIN_TIP  = 2;   // red
const uint8_t PIN_RING = 3;   // white

const uint8_t  FRAME_START    = 0x7E;
const uint8_t  MAX_PAYLOAD    = 120;
const uint32_t EDGE_TIMEOUT   = 100000UL;  // us to wait for any handshake edge
const uint32_t BYTE_TIMEOUT   = 1000000UL; // us to wait for the next byte inside a frame

// ---------- line helpers (open-collector: pull low or let go) ----------

static inline void lineLow(uint8_t pin) {
  digitalWrite(pin, LOW);
  pinMode(pin, OUTPUT);
}

static inline void lineRelease(uint8_t pin) {
  // The calculator has its own pull-ups; INPUT_PULLUP just keeps the pin
  // defined when the calculator is unplugged.
  pinMode(pin, INPUT_PULLUP);
}

static inline bool isLow(uint8_t pin) {
  return digitalRead(pin) == LOW;
}

static bool waitFor(uint8_t pin, bool wantLow, uint32_t timeoutUs) {
  uint32_t t0 = micros();
  while (isLow(pin) != wantLow) {
    if (micros() - t0 > timeoutUs) return false;
  }
  return true;
}

// ---------- bit level ----------

// Send one byte, LSB first. Returns false on timeout.
bool tiSendByte(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    // Both lines must be idle (high) before each bit.
    if (!waitFor(PIN_TIP, false, EDGE_TIMEOUT) ||
        !waitFor(PIN_RING, false, EDGE_TIMEOUT)) return false;

    if (b & 1) {
      // bit 1: pull ring, calc acks on tip
      lineLow(PIN_RING);
      if (!waitFor(PIN_TIP, true, EDGE_TIMEOUT)) { lineRelease(PIN_RING); return false; }
      lineRelease(PIN_RING);
      if (!waitFor(PIN_TIP, false, EDGE_TIMEOUT)) return false;
    } else {
      // bit 0: pull tip, calc acks on ring
      lineLow(PIN_TIP);
      if (!waitFor(PIN_RING, true, EDGE_TIMEOUT)) { lineRelease(PIN_TIP); return false; }
      lineRelease(PIN_TIP);
      if (!waitFor(PIN_RING, false, EDGE_TIMEOUT)) return false;
    }
    b >>= 1;
  }
  return true;
}

// Receive one byte, LSB first.
// firstBitTimeoutUs = 0 means "just poll": return -1 immediately if nothing is starting.
// Returns 0..255 on success, -1 if no transfer started, -2 on a mid-byte error.
int tiRecvByte(uint32_t firstBitTimeoutUs) {
  uint8_t value = 0;
  for (uint8_t i = 0; i < 8; i++) {
    uint32_t timeout = (i == 0) ? firstBitTimeoutUs : EDGE_TIMEOUT;
    uint32_t t0 = micros();
    while (!isLow(PIN_TIP) && !isLow(PIN_RING)) {
      if (micros() - t0 >= timeout) return (i == 0) ? -1 : -2;
    }

    if (isLow(PIN_TIP)) {
      // bit 0: sender pulled tip -> we ack on ring
      lineLow(PIN_RING);
      bool ok = waitFor(PIN_TIP, false, EDGE_TIMEOUT);
      lineRelease(PIN_RING);
      if (!ok) return -2;
    } else {
      // bit 1: sender pulled ring -> we ack on tip
      value |= (1 << i);
      lineLow(PIN_TIP);
      bool ok = waitFor(PIN_RING, false, EDGE_TIMEOUT);
      lineRelease(PIN_TIP);
      if (!ok) return -2;
    }
  }
  return value;
}

// ---------- frame level ----------

bool sendFrame(const uint8_t *data, uint8_t len) {
  if (len == 0 || len > MAX_PAYLOAD) return false;
  uint8_t sum = 0;
  if (!tiSendByte(FRAME_START) || !tiSendByte(len)) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (!tiSendByte(data[i])) return false;
    sum += data[i];
  }
  return tiSendByte(sum);
}

bool sendText(const char *s) {
  return sendFrame((const uint8_t *)s, strlen(s));
}

// Non-blocking check for an incoming frame.
// Returns payload length (>0) on success, 0 if nothing arrived, -1 on error.
int pollFrame(uint8_t *buf) {
  int b = tiRecvByte(0);
  if (b == -1) return 0;           // line idle
  if (b != FRAME_START) return -1; // garbage / out of sync

  int len = tiRecvByte(BYTE_TIMEOUT);
  if (len <= 0 || len > MAX_PAYLOAD) return -1;

  uint8_t sum = 0;
  for (int i = 0; i < len; i++) {
    int c = tiRecvByte(BYTE_TIMEOUT);
    if (c < 0) return -1;
    buf[i] = (uint8_t)c;
    sum += (uint8_t)c;
  }
  int chk = tiRecvByte(BYTE_TIMEOUT);
  if (chk < 0 || (uint8_t)chk != sum) return -1;
  return len;
}

// ---------- demo application ----------

char serialLine[MAX_PAYLOAD + 1];
uint8_t serialPos = 0;

void handleCalcMessage(const char *msg) {
  Serial.print(F("[calc] "));
  Serial.println(msg);

  char reply[MAX_PAYLOAD + 1];
  if (strcmp(msg, "HELLO") == 0) {
    strcpy(reply, "Hi from Arduino");
  } else if (strcmp(msg, "A0?") == 0) {
    snprintf(reply, sizeof(reply), "A0=%d", analogRead(A0));
  } else {
    snprintf(reply, sizeof(reply), "echo:%s", msg);
  }

  delay(5); // give the calc a moment to switch from sending to listening
  if (!sendText(reply)) Serial.println(F("! reply failed"));
}

void setup() {
  Serial.begin(115200);
  lineRelease(PIN_TIP);
  lineRelease(PIN_RING);
  Serial.println(F("TI link ready. Type a line to send it to the calculator."));
}

void loop() {
  // 1. Calculator -> Arduino
  uint8_t buf[MAX_PAYLOAD + 1];
  int n = pollFrame(buf);
  if (n > 0) {
    buf[n] = 0;
    handleCalcMessage((const char *)buf);
  } else if (n < 0) {
    Serial.println(F("! bad frame from calc"));
  }

  // 2. Serial Monitor -> calculator
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n' || serialPos == MAX_PAYLOAD) {
      if (serialPos > 0) {
        serialLine[serialPos] = 0;
        Serial.print(sendText(serialLine) ? F("[sent] ") : F("! send failed: "));
        Serial.println(serialLine);
      }
      serialPos = 0;
    } else {
      serialLine[serialPos++] = c;
    }
  }
}
