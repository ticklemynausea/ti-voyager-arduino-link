/*
 * TiLink.cpp - see TiLink.h
 *
 * Bit handshake (both lines idle high, open-collector):
 *   send 0: sender pulls TIP low,  receiver acks by pulling RING low,
 *           sender releases TIP,   receiver releases RING
 *   send 1: same with the roles of TIP and RING swapped
 * Bytes go LSB first. There is no clock, so timing is forgiving: each side
 * simply waits for the other, which also gives free flow control.
 */
#include "TiLink.h"

TiLink::TiLink(uint8_t tipPin, uint8_t ringPin, bool internalPullups)
  : _tip(tipPin), _ring(ringPin), _pullups(internalPullups) {}

void TiLink::begin() {
  release(_tip);
  release(_ring);
}

// ---------- line helpers: we only ever pull low or let go ----------

void TiLink::pullLow(uint8_t pin) {
  digitalWrite(pin, LOW);
  pinMode(pin, OUTPUT);
}

void TiLink::release(uint8_t pin) {
  pinMode(pin, _pullups ? INPUT_PULLUP : INPUT);
}

bool TiLink::isLow(uint8_t pin) {
  return digitalRead(pin) == LOW;
}

bool TiLink::lineIdle() {
  return !isLow(_tip) && !isLow(_ring);
}

bool TiLink::waitFor(uint8_t pin, bool wantLow, uint32_t timeoutUs) {
  uint32_t t0 = micros();
  while (isLow(pin) != wantLow) {
    if (micros() - t0 > timeoutUs) return false;
  }
  return true;
}

// Release everything, give the other side a moment to let go, count the error.
bool TiLink::fail() {
  release(_tip);
  release(_ring);
  uint32_t t0 = micros();
  while (!lineIdle() && micros() - t0 < 50000) { }
  errors++;
  return false;
}

// ---------- layer 1 ----------

bool TiLink::sendByte(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    // both lines must be idle before each bit
    if (!waitFor(_tip, false, edgeTimeoutUs) || !waitFor(_ring, false, edgeTimeoutUs))
      return fail();

    uint8_t sig = (b & 1) ? _ring : _tip;   // line we pull
    uint8_t ack = (b & 1) ? _tip  : _ring;  // line the receiver pulls

    pullLow(sig);
    if (!waitFor(ack, true, edgeTimeoutUs)) return fail();
    release(sig);
    if (!waitFor(ack, false, edgeTimeoutUs)) return fail();

    b >>= 1;
  }
  return true;
}

int TiLink::recvByte(uint32_t firstBitTimeoutUs) {
  uint8_t value = 0;
  for (uint8_t i = 0; i < 8; i++) {
    uint32_t timeout = (i == 0) ? firstBitTimeoutUs : edgeTimeoutUs;
    uint32_t t0 = micros();
    while (lineIdle()) {
      if (micros() - t0 >= timeout) {
        if (i == 0) return -1;  // nothing started: not an error
        fail();
        return -2;
      }
    }

    bool tipLow = isLow(_tip);
    bool ringLow = isLow(_ring);
    if (tipLow && ringLow) { fail(); return -2; }  // collision / garbage

    uint8_t sig = tipLow ? _tip : _ring;
    uint8_t ack = tipLow ? _ring : _tip;
    if (!tipLow) value |= (1 << i);                // ring low = bit 1

    pullLow(ack);
    bool ok = waitFor(sig, false, edgeTimeoutUs);  // sender lets go
    release(ack);
    // make sure our own ack line has actually risen before looking for the next bit
    if (!ok || !waitFor(ack, false, edgeTimeoutUs)) { fail(); return -2; }
  }
  return value;
}

// ---------- layer 2 ----------

bool TiLink::sendFrame(const uint8_t *data, uint8_t len) {
  if (len == 0 || len > MAX_PAYLOAD) return false;
  uint8_t sum = 0;
  if (!sendByte(FRAME_START) || !sendByte(len)) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (!sendByte(data[i])) return false;
    sum += data[i];
  }
  if (!sendByte(sum)) return false;
  framesSent++;
  return true;
}

bool TiLink::sendText(const char *s) {
  size_t n = strlen(s);
  if (n > MAX_PAYLOAD) n = MAX_PAYLOAD;
  return sendFrame((const uint8_t *)s, (uint8_t)n);
}

int TiLink::pollFrame(uint8_t *buf) {
  int b = recvByte(0);
  if (b == -1) return 0;                          // idle
  if (b < 0 || b != FRAME_START) { errors++; return -1; }

  int len = recvByte(byteTimeoutUs);
  if (len <= 0 || len > MAX_PAYLOAD) { errors++; return -1; }

  uint8_t sum = 0;
  for (int i = 0; i < len; i++) {
    int c = recvByte(byteTimeoutUs);
    if (c < 0) { errors++; return -1; }
    buf[i] = (uint8_t)c;
    sum += (uint8_t)c;
  }

  int chk = recvByte(byteTimeoutUs);
  if (chk < 0 || (uint8_t)chk != sum) { errors++; return -1; }

  buf[len] = 0;
  framesRecv++;
  return len;
}
