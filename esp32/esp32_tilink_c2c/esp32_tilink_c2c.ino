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
 *   core 1 (loop): reads both pins in a tight loop with the ESP32-S3's
 *                  dedicated-GPIO instruction (about 50 ns per sample), time-
 *                  stamps changes with the CPU cycle counter, and decodes the
 *                  bit handshake (BitDecoder.h) into bytes for a ring buffer.
 *                  Interrupts stay off on this core, because the calculators'
 *                  handshake phases can be about 1 us long and a timer
 *                  interrupt would hide them. They are let through briefly
 *                  every 100 ms, preferably while the link is quiet, to keep
 *                  the interrupt watchdog happy. The sampler and decoder run
 *                  from internal RAM (IRAM): code fetched through the flash
 *                  cache can stall for microseconds on a miss, which lost the
 *                  first bits of a packet after a quiet spell.
 *   core 0 (task): drains the ring buffer, assembles TI packets, checks their
 *                  checksums and prints them; also reads Serial commands.
 *
 * Serial Monitor at 115200 baud; type "help" for commands.
 */
#include "esp_attr.h"
#define BITDECODER_FN IRAM_ATTR
#include "BitDecoder.h"
#include <atomic>
#include "esp_timer.h"
#include "esp_cpu.h"
#include "driver/dedic_gpio.h"
#include "hal/dedic_gpio_cpu_ll.h"

// M5Stack Stamp S3
const int PIN_TIP  = 1;
const int PIN_RING = 5;

const uint32_t RING_SIZE = 4096;    // decoded events buffered between the cores
const uint32_t TRACE_MAX = 4096;    // line changes kept by "trace"
const uint32_t MAX_PACKET = 65536 + 6;  // id cmd len(2) data(<=65535) checksum(2)

BitDecoder dec;

// Sampler time base: CPU cycles counted from when the sampler started, which
// was `startUs` microseconds after boot (the same clock "mark" prints).
uint32_t cyclesPerUs = 240;
volatile int64_t startUs = 0;

uint64_t toUs(uint64_t cycles) { return startUs + cycles / cyclesPerUs; }
uint32_t cyclesToNs(uint32_t c) { return (uint32_t)((uint64_t)c * 1000 / cyclesPerUs); }

// "trace": the sampler records raw line changes here
struct Edge { uint64_t cycles; uint8_t lines; };   // lines: bit 1 tip, bit 0 ring (1 = high)
Edge *trace;
volatile uint32_t traceArm = 0;     // set by the printer: record this many changes
volatile uint32_t traceN = 0;       // recorded so far
volatile bool traceOn = false, traceFull = false;

// sampler health
volatile uint32_t windows = 0, forcedWindows = 0;
volatile const char *samplerError = nullptr;

// ---------- ring buffer: single producer (core 1), single consumer (core 0) ----------

LinkEvent *ring;
std::atomic<uint32_t> ringHead{0}, ringTail{0};
volatile uint32_t overflows = 0;

static inline IRAM_ATTR void push(const LinkEvent &ev) {
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
int64_t lastByteSeenUs = 0;    // when the printer last received a byte (esp_timer)

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

void printDuration(uint32_t cycles) {
  uint32_t ns = cyclesToNs(cycles);
  if (ns < 10000) Serial.printf("%lu ns", (unsigned long)ns);
  else if (ns < 10000000) Serial.printf("%lu us", (unsigned long)(ns / 1000));
  else Serial.printf("%lu ms", (unsigned long)(ns / 1000000));
}

void onByte(const LinkEvent &ev) {
  uint64_t t = toUs(ev.t);
  uint64_t dur = cyclesToNs(ev.dur) / 1000;
  lastByteSeenUs = esp_timer_get_time();
  if (pktN && t - pktEnd > gapFlushUs) flushRaw("then a pause");
  if (rawMode) {
    Serial.print("  byte ");
    printTime(t);
    Serial.printf("  %02X  ", ev.value);
    printDuration(ev.dur);
    Serial.println(ev.ambiguous ? "  AMBIGUOUS" : "");
  }
  if (pktN == 0) pktStart = t;
  if (pktN < MAX_PACKET) pkt[pktN++] = ev.value;
  pktEnd = t + dur;
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
    printTime(toUs(ev.t));
    Serial.printf(": only %u bits (so far %02X)\n", ev.bits, ev.value);
    break;
  case LinkEvent::NOACK:
    Serial.print("! at ");
    printTime(toUs(ev.t));
    Serial.print(" a line was pulled low for ");
    printDuration(ev.dur);
    Serial.printf(" and let go without an acknowledgement (%u bits into the byte)\n", ev.bits);
    break;
  case LinkEvent::STUCK:
    Serial.print("! a line has been held low since ");
    printTime(toUs(ev.t));
    Serial.println(" (waiting receiver, or unplugged?)");
    break;
  }
}

// ---------- trace ----------

void printTrace() {
  uint32_t n = traceN;
  Serial.printf("trace: %lu line changes (time from the first; tip ring, 1 = high)\n",
                (unsigned long)n);
  for (uint32_t i = 0; i < n; i++) {
    uint64_t sinceFirst = (trace[i].cycles - trace[0].cycles) * 1000 / cyclesPerUs;
    uint64_t sincePrev = i ? (trace[i].cycles - trace[i - 1].cycles) * 1000 / cyclesPerUs : 0;
    Serial.printf("  %13llu ns  +%11llu ns   %u %u\n", (unsigned long long)sinceFirst,
                  (unsigned long long)sincePrev, (trace[i].lines >> 1) & 1, trace[i].lines & 1);
  }
  traceArm = 0;   // first, so the sampler doesn't start another trace
  traceOn = false;
  traceFull = false;
  traceN = 0;
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
    "  trace [n]     record the next n line changes (default 400) and print them\n"
    "  help");
}

void printLines() {
  Serial.printf("tip %s  ring %s\n", digitalRead(PIN_TIP) ? "high" : "LOW",
                digitalRead(PIN_RING) ? "high" : "LOW");
}

void printStats() {
  Serial.printf("bytes %lu  packets %lu  bad checksums %lu\n", (unsigned long)dec.bytes,
                (unsigned long)packets, (unsigned long)badChecksums);
  Serial.printf("ambiguous bits %lu  partial bytes %lu  unacked bits %lu  held lines %lu  "
                "buffer overflows %lu\n",
                (unsigned long)dec.ambiguousBits, (unsigned long)dec.partials,
                (unsigned long)dec.noacks, (unsigned long)dec.stucks,
                (unsigned long)overflows);
  if (dec.maxBitTicks)
    Serial.printf("time between bits in a byte: min %lu ns  max %lu ns\n",
                  (unsigned long)cyclesToNs(dec.minBitTicks),
                  (unsigned long)cyclesToNs(dec.maxBitTicks));
  Serial.printf("interrupt windows %lu (forced during traffic %lu)\n", (unsigned long)windows,
                (unsigned long)forcedWindows);
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
    dec.minBitTicks = 0xFFFFFFFF;
    dec.maxBitTicks = 0;
    packets = badChecksums = overflows = windows = forcedWindows = 0;
    Serial.println("counters reset");
  } else if (cmd == "lines") {
    printLines();
  } else if (cmd == "trace") {
    long n = arg.length() ? arg.toInt() : 400;
    if (n < 1) n = 1;
    if (n > (long)TRACE_MAX) n = TRACE_MAX;
    traceN = 0;
    traceFull = false;
    traceArm = n;
    Serial.printf("trace: recording the next %ld line changes\n", n);
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
  Serial.printf("free heap %lu bytes, CPU %lu MHz\n", (unsigned long)ESP.getFreeHeap(),
                (unsigned long)cyclesPerUs);
  printLines();

  String line;
  LinkEvent ev;
  for (;;) {
    bool any = false;
    while (pop(ev)) {
      onEvent(ev);
      any = true;
    }
    if (pktN && esp_timer_get_time() - lastByteSeenUs > (int64_t)gapFlushUs) flushRaw("then a pause");
    if (traceFull) printTrace();
    if (samplerError) {
      Serial.printf("! sampler: %s\n", (const char *)samplerError);
      samplerError = nullptr;
    }
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
  cyclesPerUs = getCpuFrequencyMhz();

  ring = (LinkEvent *)malloc(RING_SIZE * sizeof(LinkEvent));
  pkt = (uint8_t *)malloc(MAX_PACKET);
  trace = (Edge *)malloc(TRACE_MAX * sizeof(Edge));
  if (!ring || !pkt || !trace) {
    Serial.begin(115200);
    for (;;) {
      Serial.println("esp32_tilink_c2c: out of memory");
      delay(2000);
    }
  }
  xTaskCreatePinnedToCore(printerTask, "printer", 8192, nullptr, 1, nullptr, 0);
}

static void sampleForever(uint32_t tipBit, uint32_t ringBit);

// Runs on core 1 and never returns.
void loop() {
  disableCore1WDT();
  // Above everything else that may run on this core except the IPC task.
  vTaskPrioritySet(nullptr, configMAX_PRIORITIES - 2);

  // A dedicated-GPIO bundle belongs to the core that creates it: this one.
  int pins[2] = {PIN_TIP, PIN_RING};
  dedic_gpio_bundle_config_t cfg = {};
  cfg.gpio_array = pins;
  cfg.array_size = 2;
  cfg.flags.in_en = 1;
  dedic_gpio_bundle_handle_t bundle = nullptr;
  uint32_t offset = 0;
  if (dedic_gpio_new_bundle(&cfg, &bundle) != ESP_OK ||
      dedic_gpio_get_in_offset(bundle, &offset) != ESP_OK) {
    samplerError = "could not set up dedicated GPIO; not sampling";
    for (;;) vTaskDelay(1000);
  }
  const uint32_t tipBit = 1UL << offset, ringBit = 1UL << (offset + 1);

  dec.midByteTicks = 50000 * cyclesPerUs;      // 50 ms
  dec.stuckTicks = 1000000 * cyclesPerUs;      // 1 s

  sampleForever(tipBit, ringBit);
}

// The sampling loop. In IRAM, like the decoder it calls (see the header comment).
static IRAM_ATTR __attribute__((noinline)) void sampleForever(const uint32_t tipBit, const uint32_t ringBit) {
  const uint32_t mask = tipBit | ringBit;
  const uint32_t windowEvery = 100000 * cyclesPerUs;   // 100 ms
  const uint32_t windowForce = 200000 * cyclesPerUs;   // 200 ms (watchdog at 300)
  const uint32_t quietBefore = 200 * cyclesPerUs;      // link idle this long = safe moment

  LinkEvent ev;
  uint32_t last = dedic_gpio_cpu_ll_read_in() & mask;
  uint32_t lastCycles = esp_cpu_get_cycle_count();
  uint64_t hi = 0;          // upper part of the 64-bit cycle count
  uint64_t lastChange = 0, lastWindow = 0;
  uint32_t spins = 0;
  startUs = esp_timer_get_time();
  const uint32_t startCycles = lastCycles;

  uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
  for (;;) {
    uint32_t v = dedic_gpio_cpu_ll_read_in() & mask;
    uint32_t c = esp_cpu_get_cycle_count();
    if (c < lastCycles) hi += 1ULL << 32;
    lastCycles = c;
    uint64_t now = (hi | c) - startCycles;

    if (v != last) {
      last = v;
      lastChange = now;
      bool tip = v & tipBit, ringHigh = v & ringBit;
      if (dec.feed(tip, ringHigh, now, ev)) push(ev);
      if (traceOn) {
        uint32_t n = traceN;
        trace[n].cycles = now;
        trace[n].lines = (tip ? 2 : 0) | (ringHigh ? 1 : 0);
        traceN = ++n;
        if (n >= traceArm) {
          traceOn = false;
          traceFull = true;
        }
      }
      continue;
    }
    if (++spins < 256) continue;
    spins = 0;
    if (traceArm && !traceOn && !traceFull && traceN == 0) traceOn = true;
    if (dec.tick(now, ev)) push(ev);

    // Let pending interrupts (the 1 kHz tick) run for a moment.
    uint64_t sinceWindow = now - lastWindow;
    bool quiet = v == mask && now - lastChange >= quietBefore;
    if ((quiet && sinceWindow >= windowEvery) || sinceWindow >= windowForce) {
      if (!quiet) forcedWindows++;
      windows++;
      portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
      __asm__ __volatile__("nop; nop; nop; nop; nop; nop; nop; nop");
      irq = portSET_INTERRUPT_MASK_FROM_ISR();
      lastWindow = now;
    }
  }
}
