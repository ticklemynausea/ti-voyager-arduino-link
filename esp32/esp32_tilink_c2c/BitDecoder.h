/*
 * BitDecoder.h - passive decoder for the TI link bit handshake
 *
 * Watches tip and ring without driving them and turns the handshake between
 * two calculators back into bytes. Plain C++ (no Arduino), so it can be tested
 * on a PC by feeding it line states.
 *
 * One bit, as seen on the wires (both lines idle high):
 *   sender pulls tip (bit 0) or ring (bit 1) low
 *   receiver acknowledges by pulling the other line low   -> both low
 *   sender releases, then receiver releases               -> both high
 * Bytes go LSB first.
 *
 * Usage: call feed() whenever the line states change, and tick() every so
 * often while they don't (for the timeouts). Each returns true when it has
 * produced an event.
 */
#pragma once
#include <stdint.h>

struct LinkEvent {
  enum Type : uint8_t {
    BYTE,      // a complete byte: value, t = start of its first bit, dur = whole byte
    PARTIAL,   // the byte stopped after `bits` bits (value holds the bits so far)
    NOACK,     // a bit ended without the receiver ever acknowledging it
    STUCK,     // notice: a bit has been held for longer than stuckUs (t = its start).
               // Only a report: the bit still completes (BYTE) or fails (NOACK) later,
               // since a busy receiver can legitimately keep the sender waiting.
  };
  Type type;
  uint8_t value;
  uint8_t bits;       // PARTIAL / NOACK: bits completed before it; BYTE: 8
  uint8_t ambiguous;  // BYTE / PARTIAL: number of bits whose line couldn't be told apart
  uint32_t t;         // microseconds
  uint32_t dur;       // microseconds
};

class BitDecoder {
public:
  uint32_t midByteTimeoutUs = 50000;   // idle gap inside a byte that abandons it
  uint32_t stuckUs = 1000000;          // a bit held this long is reported (once)

  // ---- counters (read from another core for "stats"; approximate is fine) ----
  uint32_t bytes = 0, partials = 0, noacks = 0, ambiguousBits = 0, stucks = 0;
  uint32_t minBitUs = 0xFFFFFFFF, maxBitUs = 0;

  void reset() {
    _state = WAIT_IDLE;
    _bits = 0;
    _value = 0;
    _amb = 0;
  }

  // tip / ring: true = line high. t: microseconds (wrapping is fine).
  bool feed(bool tip, bool ring, uint32_t t, LinkEvent &ev) {
    _lastChange = t;
    bool idle = tip && ring;

    switch (_state) {
    case WAIT_IDLE:  // start-up: wait until both lines are high before trusting anything
      if (idle) _state = IDLE;
      return false;

    case IDLE:
      if (idle) return false;
      _bitStart = t;
      if (_bits == 0) _byteStart = t;
      _sawBoth = !tip && !ring;
      _stuckReported = false;
      if (_sawBoth) {        // both fell between two samples: can't tell which was first
        _bitVal = 0;
        _amb++;
        ambiguousBits++;
      } else {
        _bitVal = tip ? 1 : 0;  // ring low (tip still high) = 1
      }
      _state = IN_BIT;
      return false;

    case IN_BIT:
      if (!tip && !ring) _sawBoth = true;
      if (!idle) return false;
      _state = IDLE;
      if (!_sawBoth) {       // the sender gave up waiting for an acknowledgement
        noacks++;
        uint8_t b = _bits;
        _bits = 0; _value = 0; _amb = 0;
        return make(ev, LinkEvent::NOACK, 0, b, t, t - _bitStart);
      }
      {
        uint32_t d = t - _bitStart;
        if (d < minBitUs) minBitUs = d;
        if (d > maxBitUs) maxBitUs = d;
      }
      _value |= (uint8_t)(_bitVal << _bits);
      if (++_bits < 8) return false;
      bytes++;
      {
        uint8_t v = _value, a = _amb;
        _bits = 0; _value = 0; _amb = 0;
        return make(ev, LinkEvent::BYTE, v, 8, _byteStart, t - _byteStart, a);
      }
    }
    return false;
  }

  bool tick(uint32_t t, LinkEvent &ev) {
    uint32_t quiet = t - _lastChange;
    if (_state == IN_BIT && !_stuckReported && quiet >= stuckUs) {
      _stuckReported = true;
      stucks++;
      return make(ev, LinkEvent::STUCK, 0, _bits, _bitStart, t - _bitStart);
    }
    if (_state == IDLE && _bits > 0 && quiet >= midByteTimeoutUs) {
      partials++;
      uint8_t v = _value, b = _bits, a = _amb;
      _bits = 0; _value = 0; _amb = 0;
      return make(ev, LinkEvent::PARTIAL, v, b, _byteStart, _lastChange - _byteStart, a);
    }
    return false;
  }

private:
  enum State : uint8_t { WAIT_IDLE, IDLE, IN_BIT };
  State _state = WAIT_IDLE;
  uint8_t _bits = 0, _value = 0, _bitVal = 0, _amb = 0;
  bool _sawBoth = false, _stuckReported = false;
  uint32_t _bitStart = 0, _byteStart = 0, _lastChange = 0;

  static bool make(LinkEvent &ev, LinkEvent::Type type, uint8_t value, uint8_t bits,
                   uint32_t t, uint32_t dur, uint8_t amb = 0) {
    ev.type = type;
    ev.value = value;
    ev.bits = bits;
    ev.ambiguous = amb;
    ev.t = t;
    ev.dur = dur;
    return true;
  }
};
