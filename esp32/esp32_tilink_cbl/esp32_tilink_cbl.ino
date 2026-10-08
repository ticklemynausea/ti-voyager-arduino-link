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
 * This version prints every packet and dumps the DATA bytes raw, so the
 * list encoding can be worked out from real captures.
 *
 * The machine ID the ESP32 replies with is a best guess (0x19). If the
 * calculator stops after our ACK/CTS, try another one from the Serial
 * Monitor:  id 09   id 08   id 18   id 19
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

TiLink ti(PIN_TIP, PIN_RING);
uint8_t deviceId = 0x19;

struct Packet {
  uint8_t  id, cmd;
  uint16_t len;              // length field from the header
  uint16_t stored;           // data bytes kept (capped at MAX_DATA)
  bool     checksumOk;
  uint8_t  data[MAX_DATA];
};
Packet pkt;

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

// ---------- Send {...} from the calculator ----------

void printVarHeader(const Packet &p) {
  if (p.stored < 6) return;
  uint32_t size = p.data[0] | (p.data[1] << 8) | ((uint32_t)p.data[2] << 16) |
                  ((uint32_t)p.data[3] << 24);
  uint8_t type = p.data[4], nameLen = p.data[5];
  Serial.printf("   variable: size %lu, type 0x%02X%s, name (%u bytes):",
                (unsigned long)size, type, type == 0x04 ? " (list)" : "", nameLen);
  for (uint8_t i = 0; i < nameLen && 6 + i < p.stored; i++)
    Serial.printf(" %02X", p.data[6 + i]);
  Serial.println();
}

void handleVar() {
  printVarHeader(pkt);
  if (!pkt.checksumOk) {
    Serial.println("! header checksum bad, ignoring");
    return;
  }

  if (!sendPacket(CMD_ACK)) return;
  if (!sendPacket(CMD_CTS)) return;

  if (!expect(CMD_ACK)) return;
  if (!expect(CMD_DATA)) return;
  if (!sendPacket(CMD_ACK)) return;

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
  if (cmd.startsWith("id ")) {
    deviceId = (uint8_t)strtoul(cmd.c_str() + 3, nullptr, 16);
    Serial.printf("reply machine id is now 0x%02X\n", deviceId);
  } else if (cmd.length()) {
    Serial.println("commands:  id <hex>   (e.g. id 19)");
  }
}

void setup() {
  Serial.begin(115200);
  ti.begin();
  delay(500);
  Serial.println("\nTI link CBL emulator ready.");
  Serial.printf("Line idle: %s\n", ti.lineIdle() ? "yes" : "NO - check wiring / calculator on?");
  Serial.printf("Replying with machine id 0x%02X (change with: id <hex>)\n", deviceId);
  Serial.println("Run  Send {1,2,3}  on the calculator.\n");
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
