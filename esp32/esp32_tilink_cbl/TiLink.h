/*
 * TiLink.h - TI-89 / TI-92+ / Voyage 200 link-port driver for ESP32 (and other Arduinos)
 *
 * Layer 1: the TI bit-level handshake on the two link wires (tip + ring).
 *          sendByte / recvByte; this is what the CBL sketch builds TI's
 *          packet protocol on.
 * Optional: a simple custom frame (0x7E | len | payload | checksum) for a
 *          raw-byte link with a C program on the calculator. Unused by the
 *          CBL sketch; kept from an earlier experiment.
 *
 * Wiring (Voyage 200 lines idle at 3.3 V, so an ESP32 connects directly):
 *   jack tip  (red)   -> 220 ohm -> tipPin
 *   jack ring (white) -> 220 ohm -> ringPin
 *   jack sleeve       -> GND
 * If your calculator's lines idle at about 5 V, use a bidirectional level shifter.
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
  uint32_t ackSettleUs = 50;         // time allowed for our released ack line to rise
  uint32_t framesSent = 0, framesRecv = 0, errors = 0;

  // ---- diagnostics: details of the most recent layer-1 failure ----
  // stage: 1 = recv, line went idle/timed out between bits
  //        2 = recv, both lines low at the start of a bit
  //        3 = recv, sender never released its line after our ack
  //        4 = (unused; a low ack line after release is the next bit)
  //        5 = send, lines not idle before a bit
  //        6 = send, receiver never acked
  //        7 = send, receiver never released its ack
  uint8_t lastErrStage = 0;
  uint8_t lastErrBit = 0;      // bit index 0..7 (LSB first)
  uint8_t lastErrValue = 0;    // bits collected/remaining at the time
  uint8_t lastErrTip = 1, lastErrRing = 1;  // line states when it failed
  const char *lastErrText();

private:
  void noteErr(uint8_t stage, uint8_t bit, uint8_t value);
  uint8_t _tip, _ring;
  bool _pullups;

  void pullLow(uint8_t pin);
  void release(uint8_t pin);
  bool isLow(uint8_t pin);
  bool waitFor(uint8_t pin, bool wantLow, uint32_t timeoutUs);
  bool fail();
};
