/*
 * TiLink.h - TI-89 / TI-92+ / Voyage 200 link-port driver for ESP32 (and other Arduinos)
 *
 * Layer 1: the TI bit-level handshake on the two link wires (tip + ring).
 * Layer 2: a simple frame on top:  0x7E | len (1..120) | payload | checksum
 *          (checksum = sum of payload bytes, mod 256)
 *
 * TI's own packet protocol (variables, Send/Get) is NOT used. The calculator
 * side sends/receives raw bytes with OSWriteLinkBlock / OSReadLinkBlock.
 *
 * Wiring (ESP32 is 3.3 V -> use a BSS138-style bidirectional level shifter):
 *   jack tip  (red)   -> [1k] -> shifter HV1 ; shifter LV1 -> tipPin
 *   jack ring (white) -> [1k] -> shifter HV2 ; shifter LV2 -> ringPin
 *   jack sleeve       -> GND (shared by calc, shifter and ESP32)
 *   shifter HV -> ESP32 5V, shifter LV -> ESP32 3V3
 */
#pragma once
#include <Arduino.h>

class TiLink {
public:
  static const uint8_t FRAME_START = 0x7E;
  static const uint8_t MAX_PAYLOAD = 120;

  // internalPullups: use INPUT_PULLUP when releasing a line.
  // Leave false when a level-shifter board (which has its own pull-ups) is fitted.
  TiLink(uint8_t tipPin, uint8_t ringPin, bool internalPullups = false);

  void begin();

  // ---- layer 1: single bytes ----
  // Returns true on success. On failure the lines are released and error count bumped.
  bool sendByte(uint8_t b);
  // firstBitTimeoutUs = 0 -> just poll.
  // Returns 0..255 on success, -1 if nothing started, -2 on a mid-byte error.
  int recvByte(uint32_t firstBitTimeoutUs);

  // ---- layer 2: frames ----
  bool sendFrame(const uint8_t *data, uint8_t len);
  bool sendText(const char *s);
  // Non-blocking. buf must hold MAX_PAYLOAD + 1 bytes; payload is NUL-terminated.
  // Returns payload length (>0), 0 if the line is idle, -1 on a bad/incomplete frame.
  int pollFrame(uint8_t *buf);

  bool lineIdle();

  // ---- tuning / stats ----
  uint32_t edgeTimeoutUs = 100000;   // max wait for any handshake edge
  uint32_t byteTimeoutUs = 1000000;  // max gap between bytes inside a frame
  uint32_t framesSent = 0, framesRecv = 0, errors = 0;

private:
  uint8_t _tip, _ring;
  bool _pullups;

  void pullLow(uint8_t pin);
  void release(uint8_t pin);
  bool isLow(uint8_t pin);
  bool waitFor(uint8_t pin, bool wantLow, uint32_t timeoutUs);
  bool fail();
};
