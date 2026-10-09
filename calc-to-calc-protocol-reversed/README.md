# Calc-to-calc: sniffing the link between two Voyage 200s

Goal: reverse engineer what two TI Voyage 200s say to each other over the link
cable (VAR-LINK transfers, `SendCalc` / `GetCalc`, and so on) and write it up
in [PROTOCOL.md](PROTOCOL.md).

The ESP32 sits in the middle and **only listens**: both of its pins are inputs,
so the calculators talk exactly as they would over TI's own cable.

| Path | What it is |
|---|---|
| [`../esp32/esp32_tilink_c2c/`](../esp32/esp32_tilink_c2c/) | The passive sniffer sketch: decodes the bit handshake, assembles packets, checks checksums, logs everything with timestamps. |
| [TESTS.md](TESTS.md) | The test sequence, as a checklist. |
| [PROTOCOL.md](PROTOCOL.md) | The protocol as reversed from the captures. |
| `captures/` | Raw logs, one per session. |

## Wiring

Start from the wiring in the [main README](../README.md#wiring-stamp-s3) and add
the second pigtail to the same breadboard rows:

| Breadboard row | Already there | Add |
|---|---|---|
| tip row | pigtail A red, 220 Ω to G1 | pigtail B red |
| ring row | pigtail A white, 220 Ω to G5 | pigtail B white |
| ground row | pigtail A black, Stamp GND | pigtail B black |

```
 calc A ─┬─ red ───┬───────── red ─┬─ calc B
         │         └─ 220 Ω ─ G1   │
         ├─ white ─┬──────── white ┤
         │         └─ 220 Ω ─ G5   │
         └─ black ─┴─ GND ── black ┘
```

The two pigtails joined row to row make a calculator-to-calculator cable, and
the ESP32 taps it.

- **If both calculators pull at once:** both lines are open-collector. A
  calculator can only pull a line low or let it go, never drive it high, so
  two calculators pulling together just make a low line, with no short.
  For the protocol, that is a collision, which the half-duplex,
  request/response protocol avoids. The sniffer would report one as an
  ambiguous bit.
- **The ESP32 never drives the lines:** the sketch sets G1/G5 as plain inputs.
  If a bug ever did drive a pin, the 220 Ω resistors would limit the current.
- **Power order, as before:** plug in the ESP32's USB first; unplug the
  calculators first.

### Check the wiring first

Flash `esp32/esp32_tilink_linetest` and plug in both calculators, switched
on. Both lines should read high. Then touch tip to GND for a moment with a
jumper wire, and the line test should show tip go low and back. Do the same
for ring.

## Running a capture session

Flash the sniffer:

```sh
arduino-cli compile --upload -p /dev/cu.usbmodem14301 \
  --fqbn esp32:esp32:m5stack_stamp_s3 esp32/esp32_tilink_c2c
```

Open the monitor, recording everything into a log file. macOS's `script`
keeps the monitor interactive, so you can still type commands:

```sh
script -q calc-to-calc-protocol-reversed/captures/$(date +%Y-%m-%d_%H%M%S).log \
  arduino-cli monitor -p /dev/cu.usbmodem14301 -c baudrate=115200
```

At startup it prints `tip high  ring high` once both calculators are on. Before
each test, type `mark T01` (the test's ID), then run the test from
[TESTS.md](TESTS.md). Run `stats` at the end of a session. Leave with Ctrl-C.

### Reading the output

An illustrative excerpt (the bytes are made up):

```
==== T01 ====  at     41.200317
[    42.512034 +    0.000000]  88 VAR  len 9     ck OK  1.734 ms
    size 12  type 00 expression  name "a"  rest: 00
    0000: 0C 00 00 00 00 01 61 00 ...
[    42.514101 +    0.000318]  88 ACK  len 0      0.401 ms
```

- **First column:** when the packet started, in seconds since the ESP32 booted.
- **`+`:** the gap since the previous packet ended.
- **Then:** machine ID, command, length, checksum result, and how long the
  packet took on the wire.
- **VAR, RTS and REQ headers** are decoded into size, type and name.
- **Every payload** is shown as a hex dump.

Lines starting with `!` are link-level events:

| Event | Meaning |
|---|---|
| partial byte | the sender stopped partway through a byte |
| bit never acknowledged | the receiver didn't answer and the sender gave up |
| line held low | a calculator is waiting for the other one, or a pigtail is unplugged |

The sniffer can't tell from the wires which calculator sent a packet, because
both share the same lines. That is worked out in PROTOCOL.md from the order of
the commands.

Commands: `mark <text>`, `raw on|off` (also log every byte and its timing),
`gap <ms>`, `stats`, `zero`, `lines`, `trace [n]` (record the next n raw line
changes with nanosecond timing, then print them), `help`.

### Sanity checks

- In `stats`, **ambiguous bits** should stay at 0. A non-zero count means the
  sampler saw both lines fall at once: it missed which line went first, or the
  calculators collided.
- **Buffer overflows** should stay at 0 too.
- **Bad checksums** mean the sniffer misread a byte. Note the test, and send
  the log.
