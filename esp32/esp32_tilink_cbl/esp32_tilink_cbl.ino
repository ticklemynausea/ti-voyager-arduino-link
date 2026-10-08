/*
 * esp32_tilink_cbl.ino - make the ESP32 accept TI-BASIC Send {...} like a CBL
 *
 * Speaks TI's packet protocol (layer 2), on top of TiLink's bit handshake:
 *   packet = machine id | command | length (2 bytes LE) [| data | checksum (2 bytes LE)]
 *   (only some commands carry data + checksum; ACK, CTS, EOT... are 4 bytes)
 *
 * Expected exchange for  Send {1,2,3}  on the calculator:
 *   calc -> VAR   (variable header: size, type, name)
 *   ESP  -> ACK, CTS
 *   calc -> ACK
 *   calc -> DATA  (the list contents)
 *   ESP  -> ACK
 *   calc -> EOT   (may or may not be sent)
 *   ESP  -> ACK
 *
 * SendCalc (calculator-to-calculator) uses the same exchange; only the
 * machine IDs and the DATA format differ.
 *
 * Prints every packet, dumps DATA bytes raw, and decodes what is known:
 *   lists from Send {...}   and   strings from SendCalc.
 *
 * The reply machine ID is picked automatically (see replyIdFor);
 * "id <hex>" / "id auto" in the Serial Monitor override it.
 *
 * Serial Monitor: 115200 baud, newline line ending.
 * Needs TiLink.h / TiLink.cpp in the same folder.
 */
#include "TiLink.h"

// Classic ESP32: 25 / 26.  M5Stack Stamp S3: 1 / 5.
const uint8_t PIN_TIP  = 1;
const uint8_t PIN_RING = 5;

const uint32_t REPLY_TIMEOUT_US = 2000000;  // wait for the calculator's next packet
const uint16_t MAX_DATA = 2048;

enum : uint8_t {
  CMD_VAR  = 0x06, CMD_CTS = 0x09, CMD_DATA = 0x15, CMD_SKIP = 0x36,
  CMD_ACK  = 0x56, CMD_ERR = 0x5A, CMD_RDY  = 0x68, CMD_EOT  = 0x92,
  CMD_REQ  = 0xA2, CMD_RTS = 0xC9
};

// Defined before any function: the Arduino build inserts auto-generated
// function prototypes above the first function, and they need this type.
struct Packet {
  uint8_t  id, cmd;
  uint16_t len;              // length field from the header
  uint16_t stored;           // data bytes kept (capped at MAX_DATA)
  bool     checksumOk;
  uint8_t  data[MAX_DATA];
};
Packet pkt;

TiLink ti(PIN_TIP, PIN_RING);

// Machine ID the ESP32 puts on its replies. Chosen per transfer:
//   calc sends with 0x89 (Send, CBL mode)        -> reply as a CBL, 0x19
//   calc sends with 0x88 (SendCalc, V200/92+)    -> reply with the same ID
// Both confirmed on a real Voyage 200. "id <hex>" in the Serial Monitor
// forces a fixed value instead; "id auto" goes back to automatic.
uint8_t deviceId = 0x19;
uint8_t idOverride = 0;   // 0 = automatic

uint8_t replyIdFor(uint8_t senderId) {
  if (idOverride) return idOverride;
  return (senderId == 0x89) ? 0x19 : senderId;
}

// ---------- helpers ----------

const char *cmdName(uint8_t c) {
  switch (c) {
    case CMD_VAR:  return "VAR";
    case CMD_CTS:  return "CTS";
    case CMD_DATA: return "DATA";
    case CMD_SKIP: return "SKIP";
    case CMD_ACK:  return "ACK";
    case CMD_ERR:  return "ERR";
    case CMD_RDY:  return "RDY";
    case CMD_EOT:  return "EOT";
    case CMD_REQ:  return "REQ";
    case CMD_RTS:  return "RTS";
    default:       return "?";
  }
}

bool hasData(uint8_t cmd) {
  switch (cmd) {
    case CMD_VAR: case CMD_DATA: case CMD_SKIP: case CMD_REQ: case CMD_RTS: case 0x88:
      return true;
    default:
      return false;
  }
}

void hexDump(const uint8_t *d, uint16_t n) {
  for (uint16_t i = 0; i < n; i++) {
    if (i % 16 == 0) Serial.printf("\n    %04X: ", i);
    Serial.printf("%02X ", d[i]);
  }
  Serial.println();
}

void printPacket(const char *dir, const Packet &p) {
  Serial.printf("%s %-4s id 0x%02X  len %u", dir, cmdName(p.cmd), p.id, p.len);
  if (hasData(p.cmd) && p.len > 0) {
    Serial.printf("  checksum %s", p.checksumOk ? "ok" : "BAD");
    if (p.stored < p.len) Serial.printf("  (kept first %u bytes)", p.stored);
    hexDump(p.data, p.stored);
  } else {
    Serial.println();
  }
}

// ---------- packet layer ----------

// Returns 1 = packet received, 0 = nothing arrived in time, -1 = error.
int recvPacket(Packet &p, uint32_t firstByteTimeoutUs) {
  int b = ti.recvByte(firstByteTimeoutUs);
  if (b == -1) return 0;
  if (b < 0) return -1;

  int c  = ti.recvByte(ti.byteTimeoutUs);
  int l0 = (c  < 0) ? -2 : ti.recvByte(ti.byteTimeoutUs);
  int l1 = (l0 < 0) ? -2 : ti.recvByte(ti.byteTimeoutUs);
  if (c < 0 || l0 < 0 || l1 < 0) return -1;

  p.id = (uint8_t)b;
  p.cmd = (uint8_t)c;
  p.len = (uint16_t)(l0 | (l1 << 8));
  p.stored = 0;
  p.checksumOk = true;

  if (hasData(p.cmd) && p.len > 0) {
    uint16_t sum = 0;
    for (uint16_t i = 0; i < p.len; i++) {
      int d = ti.recvByte(ti.byteTimeoutUs);
      if (d < 0) return -1;
      sum += (uint8_t)d;
      if (p.stored < MAX_DATA) p.data[p.stored++] = (uint8_t)d;
    }
    int s0 = ti.recvByte(ti.byteTimeoutUs);
    int s1 = (s0 < 0) ? -2 : ti.recvByte(ti.byteTimeoutUs);
    if (s0 < 0 || s1 < 0) return -1;
    p.checksumOk = (sum == (uint16_t)(s0 | (s1 << 8)));
  }
  return 1;
}

bool sendPacket(uint8_t cmd, const uint8_t *data = nullptr, uint16_t len = 0) {
  bool ok = ti.sendByte(deviceId) && ti.sendByte(cmd) &&
            ti.sendByte(len & 0xFF) && ti.sendByte(len >> 8);
  if (ok && data && len) {
    uint16_t sum = 0;
    for (uint16_t i = 0; ok && i < len; i++) {
      ok = ti.sendByte(data[i]);
      sum += data[i];
    }
    ok = ok && ti.sendByte(sum & 0xFF) && ti.sendByte(sum >> 8);
  }
  Serial.printf("-> %-4s id 0x%02X  len %u%s\n", cmdName(cmd), deviceId, len,
                ok ? "" : "  ! SEND FAILED");
  if (!ok) {
    Serial.printf("   link error at bit %u: %s (tip=%u ring=%u)\n", ti.lastErrBit,
                  ti.lastErrText(), ti.lastErrTip, ti.lastErrRing);
  }
  return ok;
}

// Wait for a specific packet from the calculator; prints whatever arrives.
bool expect(uint8_t cmd) {
  int r = recvPacket(pkt, REPLY_TIMEOUT_US);
  if (r == 0) {
    Serial.printf("! timed out waiting for %s\n", cmdName(cmd));
    return false;
  }
  if (r < 0) {
    Serial.printf("! link error waiting for %s at bit %u: %s\n", cmdName(cmd),
                  ti.lastErrBit, ti.lastErrText());
    return false;
  }
  printPacket("<-", pkt);
  if (pkt.cmd != cmd) {
    Serial.printf("! expected %s, got %s\n", cmdName(cmd), cmdName(pkt.cmd));
    return false;
  }
  return true;
}

// ---------- list decoding ----------
//
// DATA payload for a list sent with Send {...} (from real captures):
//   count (4 bytes, little-endian) | " v1 v2 v3 ..." (ASCII) | 00
// e.g. {-1.5,2000000,1/3} -> 03 00 00 00 " -1.5 2E+6 0.33333" 00
// The calculator converts every element to decimal text itself.

const uint16_t MAX_LIST = 64;
double   lastList[MAX_LIST];
uint16_t lastListLen = 0;

bool parseList(const Packet &p) {
  lastListLen = 0;
  if (p.stored < 5) return false;
  uint32_t count = p.data[0] | (p.data[1] << 8) | ((uint32_t)p.data[2] << 16) |
                   ((uint32_t)p.data[3] << 24);

  // copy the text part into a NUL-terminated buffer
  static char text[MAX_DATA + 1];
  uint16_t n = 0;
  for (uint16_t i = 4; i < p.stored && p.data[i] != 0; i++) text[n++] = (char)p.data[i];
  text[n] = 0;

  const char *s = text;
  while (*s && lastListLen < MAX_LIST) {
    char *end;
    double v = strtod(s, &end);
    if (end == s) break;           // no more numbers
    lastList[lastListLen++] = v;
    s = end;
  }

  if (lastListLen != count) {
    Serial.printf("! expected %lu elements, parsed %u from \"%s\"\n",
                  (unsigned long)count, lastListLen, text);
    return false;
  }
  return true;
}

void printList() {
  Serial.print("   list: {");
  for (uint16_t i = 0; i < lastListLen; i++)
    Serial.printf("%s%.10g", i ? ", " : "", lastList[i]);
  Serial.println("}");
}

// ---------- Send {...} from the calculator ----------

const char *typeName(uint8_t t) {
  switch (t) {
    case 0x04: return "list";
    case 0x0C: return "string";
    case 0x13: return "function";
    default:   return "?";
  }
}

// One character in TI's single-byte character set (used for names and
// strings), converted to UTF-8 for printing. Anything not mapped yet is
// shown as \xNN.
//   0x80-0x9F: from Wikipedia's "TI calculator character sets" table for the
//   TI-89/92/Voyage 200. Captures confirmed 80 α, 81 β, 83 γ, 85 δ, 86 ε, 87 ζ.
//   0xA0-0xFF: close to Latin-1 but not identical; not mapped yet.
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

// VAR header data: size (4 bytes LE) | type | name length | name | flag
String headerName(const Packet &p) {
  String s;
  if (p.stored < 6) return s;
  for (uint8_t i = 0; i < p.data[5] && 6 + i < p.stored; i++) s += tiChar(p.data[6 + i]);
  return s;
}

void printVarHeader(const Packet &p) {
  if (p.stored < 6) return;
  uint32_t size = p.data[0] | (p.data[1] << 8) | ((uint32_t)p.data[2] << 16) |
                  ((uint32_t)p.data[3] << 24);
  uint8_t type = p.data[4];
  Serial.printf("   variable: \"%s\"  type 0x%02X (%s)  size %lu\n", headerName(p).c_str(),
                type, typeName(type), (unsigned long)size);
}

// String sent with SendCalc (from a real capture of "six seven"):
//   00 00 00 00 | size (2 bytes, BIG-endian) | 00 | text | 00 | 2D (string tag)
// The text uses TI's character set, so each byte goes through tiChar().
bool decodeString(const Packet &p, String &out) {
  if (p.stored < 9 || p.data[6] != 0x00 || p.data[p.stored - 1] != 0x2D) return false;
  out = "";
  for (uint16_t i = 7; i < p.stored - 1 && p.data[i] != 0; i++) out += tiChar(p.data[i]);
  return true;
}

// ---------- functions: a shallow, best-effort token dump ----------
//
// Function data (from captures of f(x)=x, x^2, -x, ∞ and ζ(x)=∑(1/n^x,n,1,∞)):
//   00 00 00 00 | size (2 bytes BE) | E9 | body | E5 | params | 00 | ?? | 40 | DC
// The body is in reverse-Polish order (operands before operators) and is
// read backwards, from its end. Only tokens confirmed by captures are named;
// anything else prints as ?NN and may throw the rest of the dump off.

const char *tiVar(uint8_t t) {
  static const char *ap[] = {"a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p"};
  if (t == 0x08) return "x";
  if (t >= 0x0B && t <= 0x1A) return ap[t - 0x0B];
  return nullptr;
}

String tiToken(uint8_t t) {
  if (const char *v = tiVar(t)) return v;
  switch (t) {
    case 0x28: return "∞";
    case 0x7A: return "neg";
    case 0x93: return "^";
    case 0xBA: return "∑";
    case 0xE5: return "[";      // marks where a function's argument list starts
  }
  char buf[6];
  snprintf(buf, sizeof(buf), "?%02X", t);
  return buf;
}

bool decodeFunction(const Packet &p, String &params, String &rpn) {
  const int first = 7;                                   // first byte after E9
  if (p.stored < 13 || p.data[6] != 0xE9 || p.data[p.stored - 1] != 0xDC) return false;

  int bodyEnd = -1;                                      // the E5 closing the body
  for (int i = p.stored - 5; i >= first; i--)
    if (p.data[i] == 0xE5) { bodyEnd = i; break; }
  if (bodyEnd < 0) return false;

  params = "";
  for (int i = bodyEnd + 1; i <= (int)p.stored - 5; i++) {
    if (params.length()) params += ",";
    params += tiToken(p.data[i]);
  }

  const int MAX_TOK = 64;
  String tok[MAX_TOK];
  int n = 0;
  for (int i = bodyEnd - 1; i >= first && n < MAX_TOK; ) {
    uint8_t t = p.data[i];
    if (t == 0x1F && i - 1 >= first && i - 1 - p.data[i - 1] >= first) {
      // positive integer: value bytes | length | 1F
      uint8_t len = p.data[i - 1];
      int start = i - 1 - len;
      uint32_t v = 0;
      for (int k = start; k < i - 1; k++) v = (v << 8) | p.data[k];
      tok[n++] = String(v);
      i = start - 1;
    } else if (t == 0xF0 && i - 1 >= first) {
      // the function's own parameter: variable | F0
      tok[n++] = tiToken(p.data[i - 1]);
      i -= 2;
    } else {
      tok[n++] = tiToken(t);
      i--;
    }
  }

  rpn = "";
  for (int k = n - 1; k >= 0; k--) {
    rpn += tok[k];
    if (k) rpn += " ";
  }
  return true;
}

void handleVar() {
  printVarHeader(pkt);
  String varName = headerName(pkt);
  if (!pkt.checksumOk) {
    Serial.println("! header checksum bad, ignoring");
    return;
  }
  uint8_t varType = (pkt.stored >= 5) ? pkt.data[4] : 0xFF;
  bool cblMode = (pkt.id == 0x89);
  deviceId = replyIdFor(pkt.id);

  if (!sendPacket(CMD_ACK)) return;
  if (!sendPacket(CMD_CTS)) return;

  if (!expect(CMD_ACK)) return;
  if (!expect(CMD_DATA)) return;

  // Decode while the data is still in pkt, but print after the ACK so the
  // calculator isn't kept waiting.
  String str, params, rpn;
  bool listOk = false, strOk = false, funcOk = false;
  if (pkt.checksumOk) {
    if (varType == 0x04 && cblMode) listOk = parseList(pkt);  // Send {...}: text list
    else if (varType == 0x0C) strOk = decodeString(pkt, str);
    else if (varType == 0x13) funcOk = decodeFunction(pkt, params, rpn);
  }
  if (!sendPacket(CMD_ACK)) return;

  if (listOk) printList();
  else if (strOk) Serial.printf("   string: \"%s\"\n", str.c_str());
  else if (funcOk) Serial.printf("   function: %s(%s), body in reverse Polish: %s\n",
                                 varName.c_str(), params.c_str(), rpn.c_str());
  else Serial.printf("   (type 0x%02X: not decoded yet, raw bytes above)\n", varType);

  // An EOT may follow; acknowledge it if it does.
  int r = recvPacket(pkt, 1000000);
  if (r == 1) {
    printPacket("<-", pkt);
    if (pkt.cmd == CMD_EOT) sendPacket(CMD_ACK);
  } else if (r == 0) {
    Serial.println("   (no EOT)");
  }
  Serial.println("== transfer complete ==\n");
}

// ---------- Serial Monitor commands ----------

String line;

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd == "id auto") {
    idOverride = 0;
    Serial.println("reply machine id: automatic (0x19 for Send, sender's id for SendCalc)");
  } else if (cmd.startsWith("id ")) {
    idOverride = (uint8_t)strtoul(cmd.c_str() + 3, nullptr, 16);
    Serial.printf("reply machine id forced to 0x%02X\n", idOverride);
  } else if (cmd.length()) {
    Serial.println("commands:  id auto   |   id <hex>   (e.g. id 88)");
  }
}

void setup() {
  Serial.begin(115200);
  ti.begin();
  delay(500);
  Serial.println("\nTI link CBL emulator ready.");
  Serial.printf("Line idle: %s\n", ti.lineIdle() ? "yes" : "NO - check wiring / calculator on?");
  Serial.println("Reply machine id: automatic (override with: id <hex>, back with: id auto)");
  Serial.println("Try  Send {1,2,3}  or  \"hello\"->s : SendCalc s  on the calculator.\n");
}

void loop() {
  int r = recvPacket(pkt, 0);
  if (r == 1) {
    printPacket("<-", pkt);
    if (pkt.cmd == CMD_VAR) handleVar();
    else if (pkt.cmd == CMD_RDY || pkt.cmd == CMD_EOT) sendPacket(CMD_ACK);
  } else if (r < 0) {
    Serial.printf("! link error at bit %u: %s (tip=%u ring=%u)\n", ti.lastErrBit,
                  ti.lastErrText(), ti.lastErrTip, ti.lastErrRing);
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
}
