# ti-voyager-arduino-link

Experiments in talking to a **TI Voyage 200** graphing calculator through its 2.5 mm
link port, from an Arduino or ESP32.

The long-term idea is to give the Voyage 200 some form of internet access, with an
ESP32 doing the networking and the calculator acting as a terminal. This repo is
the exploratory groundwork, not a finished product: expect rough edges, and treat
the protocol notes below as findings from one calculator rather than a specification.

## Hardware

- TI Voyage 200 (the TI-89 / TI-92 Plus family should behave the same way)
- M5Stack Stamp S3 (ESP32-S3); a classic ESP32 or an Arduino Uno/Nano also work
  with different pins
- A 2.5 mm stereo (TRS) pigtail cable. The plug's moulded collar may need shaving
  down to seat fully in the Voyage 200's recessed port.
- Two 220 ohm resistors

## Wiring (Stamp S3)

The Voyage 200's link lines are open-collector with pull-ups, and on this unit they
idle at **3.3 V**, so the Stamp S3 is wired directly with no level shifter.

| Pigtail wire | Plug part | Stamp S3 |
|---|---|---|
| Red | Tip | 220 ohm -> G1 |
| White | Ring | 220 ohm -> G5 |
| Black | Sleeve | GND |

Measure your own calculator first (tip and ring against sleeve, calculator on, at
the Home screen). If the lines idle at about 5 V, use a bidirectional level shifter.

Things learned the hard way:

- **A solid ground connection is essential.** With a loose ground the lines pick up
  hum from the laptop's power adapter and toggle every 10 ms.
- **Power order matters.** Plug in the ESP32's USB before connecting the calculator,
  and unplug the calculator first. Otherwise the calculator's pull-ups can
  half-power the unpowered ESP32 through its pin protection diodes, and it then
  fails to appear on USB.
- Stranded pigtail wires make poor breadboard contacts; tin them or solder them to
  header pins.

## Contents

| Path | What it is |
|---|---|
| `esp32/esp32_tilink_cbl/` | **Current main experiment.** Makes the ESP32 answer TI-BASIC `Send {...}` like a CBL, and `SendCalc` like another calculator, then decodes lists, strings and (shallowly) functions. |
| `esp32/esp32_tilink_sniffer/` | Receive-only sniffer: acknowledges every byte and prints TI packets in hex. |
| `esp32/esp32_tilink_linetest/` | Prints raw tip/ring states; for checking the wiring. |
| `esp32/esp32_tilink_pingpong/` | Ping-pong demo using a simple custom frame format. Needs `calculator/pingpong.c` on the calculator. |
| `arduino/ti_link_arduino/` | The first experiment: an Arduino Uno/Nano version of the raw-byte link. |
| `calculator/` | C programs for the calculator side (GCC4TI / TIGCC). Not yet tested on hardware. |

Each ESP32 sketch folder carries its own copy of `TiLink.h` / `TiLink.cpp`, the
link-layer library (the bit-level handshake plus error diagnostics). Keep the
copies in sync when changing it.

## Building and flashing

With `arduino-cli` and the ESP32 board package installed:

```sh
arduino-cli compile --upload -p /dev/cu.usbmodem14301 \
  --fqbn esp32:esp32:m5stack_stamp_s3 esp32/esp32_tilink_cbl
arduino-cli monitor -p /dev/cu.usbmodem14301 -c baudrate=115200
```

Replace the port with your own (`arduino-cli board list`). The sketches also open
in the Arduino IDE.

Calculator programs build with GCC4TI, e.g. `tigcc -Os -o pingpong calculator/pingpong.c`,
and are sent to the calculator with TiLP and a link cable.

## Protocol findings

Observed on a Voyage 200 with the sketches in this repo:

- **Bit handshake.** Both lines idle high. A 0 bit is signalled by the sender
  pulling tip low, a 1 bit by pulling ring low; the receiver acknowledges on the
  other line. Bytes go LSB first. The calculator reacts very quickly: after the
  receiver releases its acknowledgement line, the calculator may pull that same
  line low for the next bit before the ESP32 ever reads it as high, so the receiver
  must not insist on seeing it rise.
- **Packets.** `machine id | command | length (2 bytes, little-endian)`, followed
  for some commands by `data | checksum (16-bit sum, little-endian)`.
- **`Send {...}`** (CBL mode). The calculator sends with machine ID `0x89` and
  accepts replies from `0x19`. Exchange: VAR -> ACK, CTS -> ACK -> DATA -> ACK ->
  EOT -> ACK. The DATA payload is a 4-byte element count followed by the elements
  as decimal text, e.g. `03 00 00 00 " -1.5 2E+6 0.33333" 00`. Only numbers are
  accepted, and values are rounded to about 5 significant digits.
- **`SendCalc`** uses the same exchange with machine ID `0x88`, and needs replies
  carrying `0x88` too. A string arrives as
  `00 00 00 00 | size (2 bytes, big-endian) | 00 | text | 00 | 2D`.
- **Functions** (`SendCalc` of a function, type `0x13`) arrive in the calculator's
  internal tokenized form, not as text:
  `00 00 00 00 | size (2 bytes BE) | E9 | body | E5 | parameters | 00 | ?? | 40 | DC`.
  The body is in reverse-Polish order (operands before operators). Tokens confirmed
  from captures: `08` x, `0B`-`1A` the letters a-p (`18` = n), `08 F0` x as the
  function's own parameter, `<value> <length> 1F` a positive integer, `28` ∞,
  `7A` negate, `93` ^, `BA` ∑, and `E5` marking where ∑'s arguments start. For
  example ζ(x) = ∑(1/n^x, n, 1, ∞) is stored as `n^(-x)`:
  `E5 28 | 01 01 1F | 18 | 08 F0 7A 18 93 | BA`. The CBL sketch prints function
  bodies as a token list using these; it does not rebuild the formula.
- **Not done yet:** `Get` (the ESP32 sending values back to the calculator).

### Character set

The calculator does not use Unicode. Names and text use TI's own 8-bit character
set: one byte per character, with `0x20`-`0x7E` matching ASCII and TI's own
symbols (Greek letters, math symbols, accented letters) from `0x80` up.

**This is a known gap.** Only one non-ASCII code is confirmed: `0x87` = ζ, from a
function named ζ. The sketches therefore print any other code above `0x7E` as
`\xNN`. Greek letters probably run in order from `0x80` (α, β, Γ, γ, Δ, δ, ε, ζ…),
which fits ζ at `0x87`, but that is unverified. To fill in the table, store a
string containing the characters of interest, send it with `SendCalc`, and read
the codes from the hex dump. Whether strings use the same codes as variable
names is also still to be confirmed.

### Open questions

- The last byte of the VAR header is `00` for strings and `01` for functions whose
  body uses the parameter, but `00` for `f(x)=∞`. Its meaning is unknown.
- In function data, the byte before `40 DC` was `01` only for ζ (perhaps related
  to ∑'s local variable `n`), and the meaning of `40` is unknown.
- Byte order of integers wider than one byte has not been checked.

## Credits

- [ArTICL](https://github.com/KermMartian/ArTICL) by Christopher Mitchell, the
  Arduino TI linking library, for the pin conventions and approach.
- The [TIGCC documentation](http://tigcc.ticalc.org/doc/link.html) for the
  calculator's link functions and variable types.
