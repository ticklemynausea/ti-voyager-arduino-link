/*
 * esp32_tilink_c2c.ino - passive sniffer for the link between two TI calculators
 *
 * Taps the tip and ring lines of a calculator-to-calculator connection and
 * logs every packet the two calculators exchange. It NEVER drives the lines:
 * both pins are plain inputs, so the calculators talk exactly as they would
 * over TI's own cable.
 *
 * Wiring (see calc-to-calc-protocol-reversed/README.md): both pigtails' tips joined and taken through
 * 220 ohm to G1; both rings joined, through 220 ohm to G5; all grounds joined
 * to GND.
 *
 * Structure:
 *   core 1 (loop): polls the GPIO input register in a tight loop and decodes
 *                  the bit handshake (BitDecoder.h) into bytes, which go into
 *                  a ring buffer. It never prints, so it never misses an edge.
 *   core 0 (task): drains the ring buffer, assembles TI packets, checks their
 *                  checksums and prints them; also reads Serial commands.
 *
 * Serial Monitor at 115200 baud; type "help" for commands.
 */
#include "BitDecoder.h"
#include <atomic>
#include "esp_timer.h"
#include "soc/gpio_reg.h"

// M5Stack Stamp S3. Both pins must be below 32 (read from GPIO_IN_REG).
const uint8_t PIN_TIP  = 1;
const uint8_t PIN_RING = 5;
const uint32_t TIP_MASK  = 1UL << PIN_TIP;
const uint32_t RING_MASK = 1UL << PIN_RING;

const uint32_t RING_SIZE = 8192;    // decoded events buffered between the cores
const uint32_t MAX_PACKET = 65536 + 6;  // id cmd len(2) data(<=65535) checksum(2)

BitDecoder dec;

// ---------- ring buffer: single producer (core 1), single consumer (core 0) ----------

LinkEvent *ring;
std::atomic<uint32_t> ringHead{0}, ringTail{0};
volatile uint32_t overflows = 0;

static inline void push(const LinkEvent &ev) {
  uint32_t h = ringHead.load(std::memory_order_relaxed);
  uint32_t next = (h + 1) % RING_SIZE;
  if (next == ringTail.load(std::memory_order_acquire)) {
    overflows++;
    return;
  }
  ring[h] = ev;
  ringHead.store(next, std::memory_order_release);
}

static bool pop(LinkEvent &ev) {
  uint32_t t = ringTail.load(std::memory_order_relaxed);
  if (t == ringHead.load(std::memory_order_acquire)) return false;
  ev = ring[t];
  ringTail.store((t + 1) % RING_SIZE, std::memory_order_release);
  return true;
}

// ---------- names ----------

const char *cmdName(uint8_t c) {
  switch (c) {
    case 0x06: return "VAR";
    case 0x09: return "CTS";
    case 0x15: return "DATA";
    case 0x2D: return "VER";
    case 0x36: return "SKIP";
    case 0x56: return "ACK";
    case 0x5A: return "ERR";
    case 0x68: return "RDY";
    case 0x6D: return "SCR";
    case 0x78: return "CONT";
    case 0x87: return "CMD";
    case 0x88: return "DEL";
    case 0x92: return "EOT";
    case 0xA2: return "REQ";
    case 0xC9: return "RTS";
    default:   return "?";
  }
}

// Packets that are only the 4-byte header, even when the length field is not 0.
bool headerOnly(uint8_t c) {
  switch (c) {
    case 0x09: case 0x56: case 0x5A: case 0x68: case 0x6D: case 0x78: case 0x92:
      return true;
    default:
      return false;
  }
}

// Variable types in VAR/RTS/REQ headers. 04, 0C, 13 are confirmed by this
// repo's captures; the rest are from the TI-89/92+ link guide and still to be
// confirmed by calc-to-calc captures.
const char *typeName(uint8_t t) {
  switch (t) {
    case 0x00: return "expression";
    case 0x04: return "list";
    case 0x06: return "matrix";
    case 0x0A: return "data";
    case 0x0B: return "text";
    case 0x0C: return "string";
    case 0x0D: return "gdb";
    case 0x0E: return "figure";
    case 0x10: return "picture";
    case 0x12: return "program";
    case 0x13: return "function";
    case 0x14: return "macro";
    case 0x1D: return "backup";
    case 0x1F: return "folder";
    case 0x21: return "asm";
    case 0x23: return "os";
    case 0x24: return "flash app";
    default:   return "?";
  }
}

// TI single-byte character set -> UTF-8 (0x80-0x9F from the main README's
// table). Unmapped codes print as \xNN.
String tiChar(uint8_t c) {
  static const char *hi[] = {
    "α", "β", "Γ", "γ", "Δ", "δ", "ε", "ζ", "θ", "λ", "ξ", "∏", "π", "ρ", "∑", "σ",  // 80
    "τ", "φ", "ψ", "Ω", "ω", "ᴇ", "ℯ", "𝐢", "ʳ", "ᵀ", "x̅", "y̅", "≤", "≠", "≥", "∠",  // 90
  };
  if (c >= 0x20 && c < 0x7F) return String((char)c);
  if (c >= 0x80 && c <= 0x9F) return hi[c - 0x80];
  char buf[8];
  snprintf(buf, sizeof(buf), "\\x%02X", c);
  return buf;
}

// ---------- time ----------

// Events carry 32-bit microseconds (wrap every 71 minutes); extend to 64 bits.
uint64_t extend(uint32_t t32) {
  static uint32_t hi = 0, last = 0;
  if (t32 < last && last - t32 > 0x80000000UL) hi++;
  last = t32;
  return ((uint64_t)hi << 32) | t32;
}

void printTime(uint64_t us) {
  Serial.printf("%6lu.%06lu", (unsigned long)(us / 1000000), (unsigned long)(us % 1000000));
}

// ---------- packet assembly and printing ----------

uint8_t *pkt;
uint32_t pktN = 0;
uint64_t pktStart = 0, pktEnd = 0, prevPktEnd = 0;
uint32_t pktAmbiguous = 0;
uint32_t packets = 0, badChecksums = 0;
bool rawMode = false;
uint32_t gapFlushUs = 500000;  // leftover bytes this old are printed as "raw"

uint32_t expectedLen() {
  if (pktN < 4) return 4;
  uint16_t len = pkt[2] | (pkt[3] << 8);
  if (headerOnly(pkt[1]) || len == 0) return 4;
  return 4 + len + 2;
}

void hexDump(const uint8_t *d, uint32_t n) {
  for (uint32_t i = 0; i < n; i += 16) {
    Serial.printf("    %04lX: ", (unsigned long)i);
    for (uint32_t j = 0; j < 16; j++) {
      if (i + j < n) Serial.printf("%02X ", d[i + j]);
      else Serial.print("   ");
    }
    Serial.print(" ");
    for (uint32_t j = 0; j < 16 && i + j < n; j++) {
      uint8_t c = d[i + j];
      Serial.print((c >= 0x20 && c < 0x7F) ? (char)c : '.');
    }
    Serial.println();
  }
}

// VAR / RTS / REQ data: size (4 LE) | type | name length | name | rest
void printVarHeader(const uint8_t *d, uint16_t n) {
  if (n < 6) return;
  uint32_t size = d[0] | (d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
  uint8_t type = d[4], nameLen = d[5];
  String name;
  for (uint16_t i = 0; i < nameLen && 6 + i < n; i++) name += tiChar(d[6 + i]);
  Serial.printf("    size %lu  type %02X %s  name \"%s\"", (unsigned long)size, type,
                typeName(type), name.c_str());
  uint16_t rest = 6 + nameLen;
  if (rest < n) {
    Serial.print("  rest:");
    for (uint16_t i = rest; i < n; i++) Serial.printf(" %02X", d[i]);
  }
  Serial.println();
}

void printPacketLine(const char *label) {
  Serial.print("[");
  printTime(pktStart);
  if (prevPktEnd) {
    Serial.print(" +");
    printTime(pktStart - prevPktEnd);
  }
  Serial.printf("]  %s", label);
}

void finishPacket() {
  uint16_t len = pkt[2] | (pkt[3] << 8);
  bool withData = !headerOnly(pkt[1]) && len > 0;
  packets++;

  printPacketLine("");
  Serial.printf("%02X %-4s len %-5u", pkt[0], cmdName(pkt[1]), len);
  if (headerOnly(pkt[1]) && len) Serial.print(" (header only)");
  if (withData) {
    uint16_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += pkt[4 + i];
    uint16_t got = pkt[4 + len] | (pkt[5 + len] << 8);
    bool ok = sum == got;
    if (!ok) badChecksums++;
    Serial.printf(" ck %s", ok ? "OK" : "BAD");
    if (!ok) Serial.printf(" (sum %04X, sent %04X)", sum, got);
  }
  uint64_t dur = pktEnd - pktStart;
  Serial.printf("  %lu.%03lu ms", (unsigned long)(dur / 1000), (unsigned long)(dur % 1000));
  if (pktN >= 64 && dur) Serial.printf("  %.0f B/s", pktN * 1e6 / dur);
  if (pktAmbiguous) Serial.printf("  ! %lu AMBIGUOUS bits", (unsigned long)pktAmbiguous);
  Serial.println();

  if (withData) {
    const uint8_t *d = pkt + 4;
    uint8_t c = pkt[1];
    if (c == 0x06 || c == 0xC9 || c == 0xA2) printVarHeader(d, len);
    else if (c == 0x36) Serial.printf("    reason %02X\n", d[0]);
    hexDump(d, len);
  }
  prevPktEnd = pktEnd;
  pktN = 0;
  pktAmbiguous = 0;
}

void flushRaw(const char *why) {
  if (!pktN) return;
  printPacketLine("");
  Serial.printf("raw: %lu byte%s (%s)", (unsigned long)pktN, pktN == 1 ? "" : "s", why);
  if (pktAmbiguous) Serial.printf("  ! %lu AMBIGUOUS bits", (unsigned long)pktAmbiguous);
  Serial.println();
  hexDump(pkt, pktN);
  prevPktEnd = pktEnd;
  pktN = 0;
  pktAmbiguous = 0;
}

void onByte(const LinkEvent &ev) {
  uint64_t t = extend(ev.t);
  if (pktN && t - pktEnd > gapFlushUs) flushRaw("then a pause");
  if (rawMode) {
    Serial.print("  byte ");
    printTime(t);
    Serial.printf("  %02X  %lu us%s\n", ev.value, (unsigned long)ev.dur,
                  ev.ambiguous ? "  AMBIGUOUS" : "");
  }
  if (pktN == 0) pktStart = t;
  if (pktN < MAX_PACKET) pkt[pktN++] = ev.value;
  pktEnd = t + ev.dur;
  pktAmbiguous += ev.ambiguous;
  if (pktN == expectedLen()) finishPacket();
}

void onEvent(const LinkEvent &ev) {
  switch (ev.type) {
  case LinkEvent::BYTE:
    onByte(ev);
    break;
  case LinkEvent::PARTIAL:
    flushRaw("before a partial byte");
    Serial.print("! partial byte at ");
    printTime(extend(ev.t));
    Serial.printf(": only %u bits (so far %02X)\n", ev.bits, ev.value);
    break;
  case LinkEvent::NOACK:
    Serial.print("! a bit was never acknowledged; sender let go after ");
    Serial.printf("%lu.%03lu ms (%u bits into the byte)\n", (unsigned long)(ev.dur / 1000),
                  (unsigned long)(ev.dur % 1000), ev.bits);
    break;
  case LinkEvent::STUCK:
    Serial.print("! a line has been held low since ");
    printTime(extend(ev.t));
    Serial.println(" (waiting receiver, or unplugged?)");
    break;
  }
}

// ---------- commands ----------

void printHelp() {
  Serial.println(
    "commands:\n"
    "  mark <text>   write a ==== <text> ==== line into the log\n"
    "  raw on|off    also print every byte with its timing\n"
    "  gap <ms>      pause after which leftover bytes are printed as raw\n"
    "  stats         counters and bit timing\n"
    "  zero          reset the counters\n"
    "  lines         show the current tip/ring levels\n"
    "  help");
}

void printLines() {
  uint32_t v = REG_READ(GPIO_IN_REG);
  Serial.printf("tip %s  ring %s\n", (v & TIP_MASK) ? "high" : "LOW",
                (v & RING_MASK) ? "high" : "LOW");
}

void printStats() {
  Serial.printf("bytes %lu  packets %lu  bad checksums %lu\n", (unsigned long)dec.bytes,
                (unsigned long)packets, (unsigned long)badChecksums);
  Serial.printf("ambiguous bits %lu  partial bytes %lu  unacked bits %lu  held lines %lu  "
                "buffer overflows %lu\n",
                (unsigned long)dec.ambiguousBits, (unsigned long)dec.partials,
                (unsigned long)dec.noacks, (unsigned long)dec.stucks,
                (unsigned long)overflows);
  if (dec.maxBitUs)
    Serial.printf("bit time min %lu us  max %lu us\n", (unsigned long)dec.minBitUs,
                  (unsigned long)dec.maxBitUs);
  printLines();
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String arg = (sp < 0) ? "" : line.substring(sp + 1);
  cmd.toLowerCase();
  arg.trim();

  if (cmd == "mark") {
    flushRaw("before a mark");
    Serial.print("\n==== ");
    Serial.print(arg);
    Serial.print(" ====  at ");
    printTime(esp_timer_get_time());
    Serial.println();
  } else if (cmd == "raw") {
    rawMode = (arg == "on");
    Serial.printf("raw %s\n", rawMode ? "on" : "off");
  } else if (cmd == "gap" && arg.toInt() > 0) {
    gapFlushUs = arg.toInt() * 1000UL;
    Serial.printf("gap %lu ms\n", (unsigned long)(gapFlushUs / 1000));
  } else if (cmd == "stats") {
    printStats();
  } else if (cmd == "zero") {
    dec.bytes = dec.partials = dec.noacks = dec.ambiguousBits = dec.stucks = 0;
    dec.minBitUs = 0xFFFFFFFF;
    dec.maxBitUs = 0;
    packets = badChecksums = overflows = 0;
    Serial.println("counters reset");
  } else if (cmd == "lines") {
    printLines();
  } else {
    printHelp();
  }
}

// ---------- core 0: printer ----------

void printerTask(void *) {
  // Serial is started here so its USB interrupt lands on core 0, away from the sampler.
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nesp32_tilink_c2c: passive TI link sniffer (tip G1, ring G5). Type help.");
  Serial.printf("free heap %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  printLines();

  String line;
  LinkEvent ev;
  for (;;) {
    bool any = false;
    while (pop(ev)) {
      onEvent(ev);
      any = true;
    }
    if (pktN && (uint64_t)esp_timer_get_time() - pktEnd > gapFlushUs) flushRaw("then a pause");
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        handleCommand(line);
        line = "";
      } else {
        line += c;
      }
    }
    if (!any) vTaskDelay(1);
  }
}

// ---------- core 1: sampler ----------

void setup() {
  pinMode(PIN_TIP, INPUT);   // inputs only, no pull-ups: the calculators provide them
  pinMode(PIN_RING, INPUT);

  ring = (LinkEvent *)malloc(RING_SIZE * sizeof(LinkEvent));
  pkt = (uint8_t *)malloc(MAX_PACKET);
  if (!ring || !pkt) {
    Serial.begin(115200);
    for (;;) {
      Serial.println("esp32_tilink_c2c: out of memory");
      delay(2000);
    }
  }
  xTaskCreatePinnedToCore(printerTask, "printer", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  // Runs on core 1 and never returns.
  disableCore1WDT();
  uint32_t last = REG_READ(GPIO_IN_REG) & (TIP_MASK | RING_MASK);
  uint32_t spins = 0;
  LinkEvent ev;
  for (;;) {
    uint32_t v = REG_READ(GPIO_IN_REG) & (TIP_MASK | RING_MASK);
    if (v != last) {
      last = v;
      if (dec.feed(v & TIP_MASK, v & RING_MASK, (uint32_t)esp_timer_get_time(), ev)) push(ev);
    } else if (++spins >= 4096) {
      spins = 0;
      if (dec.tick((uint32_t)esp_timer_get_time(), ev)) push(ev);
    }
  }
}
