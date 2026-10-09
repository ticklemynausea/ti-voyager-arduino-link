/*
 * BitDecoder.h - passive decoder for the TI link bit handshake
 *
 * Watches tip and ring without driving them and turns the handshake between
 * two calculators back into bytes. Plain C++ (no Arduino), so it can be tested
 * on a PC by feeding it line states.
 *
 * One bit, as seen on the wires (both lines idle high):
 *   START  sender pulls tip (bit 0) or ring (bit 1) low
 *   BOTH   receiver acknowledges by pulling the other line low
 *   END    one side lets go, so a single line is low again
 *   IDLE   the other side lets go: both high
 * Bytes go LSB first.
 *
 * The calculators can be very quick: the idle moment between two bits can
 * last only a microsecond or so, and the next bit may even start on the line
 * that was just released. So bits are not counted on idle. Each bit has
 * exactly one BOTH phase, so a bit is counted on entering BOTH, and its value
 * is the line that was low on its own just before that.
 *
 * Time is in arbitrary "ticks" (microseconds in the PC test, CPU cycles on the
 * ESP32); set the two timeouts in the same unit.
 *
 * Usage: call feed() whenever the line states change, and tick() every so
 * often while they don't (for the timeouts). Each returns true when it has
 * produced an event.
 */
#pragma once
#include <stdint.h>

struct LinkEvent {
  uint64_t t;       // BYTE / PARTIAL: start of the byte; NOACK / STUCK: start of the bit
  uint32_t dur;     // BYTE: the whole byte; PARTIAL: until the last bit; NOACK / STUCK: the bit so far
  enum Type : uint8_t {
    BYTE,      // a complete byte
    PARTIAL,   // the byte stopped after `bits` bits (value holds the bits so far)
    NOACK,     // a line was pulled and let go without the other side acknowledging
    STUCK,     // notice: a bit has been in progress for longer than stuckTicks.
               // Only a report: the bit still completes or fails later, since a
               // busy receiver can legitimately keep the sender waiting.
  } type;
  uint8_t value;
  uint8_t bits;       // bits completed in the byte (BYTE: 8)
  uint8_t ambiguous;  // bits in the byte whose line couldn't be told apart
};

class BitDecoder {
public:
  uint32_t midByteTicks = 50000;    // quiet time inside a byte that abandons it
  uint32_t stuckTicks = 1000000;    // a bit in progress this long is reported (once)

  // ---- counters (read from another core for "stats"; approximate is fine) ----
  uint32_t bytes = 0, partials = 0, noacks = 0, ambiguousBits = 0, stucks = 0;
  uint32_t minBitTicks = 0xFFFFFFFF, maxBitTicks = 0;   // between bits inside a byte

  // tip / ring: true = line high. Call only when at least one of them changed.
  bool feed(bool tip, bool ring, uint64_t t, LinkEvent &ev) {
    uint8_t s = (tip ? 2 : 0) | (ring ? 1 : 0);   // 3 idle, 1 tip low, 2 ring low, 0 both low
    _lastChange = t;

    if (_phase == WAIT_IDLE) {   // start-up: trust nothing until both lines are high
      if (s == 3) _phase = IDLE;
      return false;
    }

    switch (s) {
    case 3:   // idle
      if (_phase == START) {     // pulled and let go without an acknowledgement
        noacks++;
        uint8_t b = _bits;
        clearByte();
        _phase = IDLE;
        return make(ev, LinkEvent::NOACK, 0, b, 0, _bitStart, t - _bitStart);
      }
      _phase = IDLE;
      return false;

    case 1: case 2:   // one line low
      if (_phase == IDLE) {
        beginBit(t);
        _phase = START;
      } else if (_phase == BOTH) {
        _phase = END;
      } else if (s != _single) {
        if (_phase == END) {
          // the other line, straight after END: the idle moment was too short to
          // see, and a new bit has started on the other line
          beginBit(t);
          _phase = START;
        } else {
          // START on one line, then the other line alone: BOTH was too short to
          // see. That completes this bit.
          uint8_t v = _bitVal(_single);
          _single = s;
          _phase = END;
          return completeBit(v, t, ev);
        }
      }
      _single = s;
      return false;

    case 0:   // both low: the acknowledgement, once per bit
    default: {
      uint8_t v;
      bool amb = false;
      if (_phase == START) {
        v = _bitVal(_single);
      } else if (_phase == END) {
        // a new bit started on the line still held from the previous one,
        // before we saw it go high
        beginBit(t);
        v = _bitVal(_single);
      } else if (_phase == IDLE) {
        // both fell between two samples: can't tell which was first
        beginBit(t);
        v = 0;
        amb = true;
      } else {
        return false;   // already BOTH
      }
      _phase = BOTH;
      if (amb) {
        _amb++;
        ambiguousBits++;
      }
      return completeBit(v, t, ev);
    }
    }
  }

  bool tick(uint64_t t, LinkEvent &ev) {
    uint64_t quiet = t - _lastChange;
    bool inBit = _phase == START || _phase == BOTH;
    if (inBit && !_stuckReported && quiet >= stuckTicks) {
      _stuckReported = true;
      stucks++;
      return make(ev, LinkEvent::STUCK, 0, _bits, 0, _bitStart, (uint32_t)(t - _bitStart));
    }
    if (!inBit && _bits > 0 && quiet >= midByteTicks) {
      partials++;
      uint8_t v = _value, b = _bits, a = _amb;
      clearByte();
      return make(ev, LinkEvent::PARTIAL, v, b, a, _byteStart, (uint32_t)(_lastBitAt - _byteStart));
    }
    return false;
  }

private:
  enum Phase : uint8_t { WAIT_IDLE, IDLE, START, BOTH, END };
  Phase _phase = WAIT_IDLE;
  uint8_t _single = 3;   // which line was low on its own most recently (1 tip, 2 ring)
  uint8_t _bits = 0, _value = 0, _amb = 0;
  bool _stuckReported = false;
  uint64_t _bitStart = 0, _byteStart = 0, _lastChange = 0, _lastBitAt = 0;

  // tip low alone (1) means a 0 bit; ring low alone (2) means a 1 bit
  static uint8_t _bitVal(uint8_t single) { return single == 2 ? 1 : 0; }

  void beginBit(uint64_t t) {
    _bitStart = t;
    _stuckReported = false;
    if (_bits == 0) _byteStart = t;
  }

  void clearByte() {
    _bits = 0;
    _value = 0;
    _amb = 0;
  }

  bool completeBit(uint8_t v, uint64_t t, LinkEvent &ev) {
    if (_bits > 0) {
      uint64_t d = t - _lastBitAt;
      if (d < minBitTicks) minBitTicks = (uint32_t)d;
      if (d > maxBitTicks) maxBitTicks = (uint32_t)(d > 0xFFFFFFFF ? 0xFFFFFFFF : d);
    }
    _lastBitAt = t;
    _value |= (uint8_t)(v << _bits);
    if (++_bits < 8) return false;
    bytes++;
    uint8_t value = _value, a = _amb;
    clearByte();
    return make(ev, LinkEvent::BYTE, value, 8, a, _byteStart, (uint32_t)(t - _byteStart));
  }

  static bool make(LinkEvent &ev, LinkEvent::Type type, uint8_t value, uint8_t bits,
                   uint8_t amb, uint64_t t, uint64_t dur) {
    ev.type = type;
    ev.value = value;
    ev.bits = bits;
    ev.ambiguous = amb;
    ev.t = t;
    ev.dur = dur > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)dur;
    return true;
  }
};
