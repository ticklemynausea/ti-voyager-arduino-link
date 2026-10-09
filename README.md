# ti-voyager-arduino-link

Experiments in talking to a **TI Voyage 200** graphing calculator through its
2.5 mm link port, using an ESP32 (M5Stack Stamp S3) programmed with the Arduino
framework.

The long-term idea is to give the Voyage 200 some form of internet access, with
the ESP32 doing the networking and the calculator acting as a terminal. This
repo is the groundwork for that, not a finished product. It holds two
experiments:

1. **ESP32 as a link partner.** The ESP32 takes part in the link, posing as a
   CBL data logger or as another calculator. TI-BASIC's `Send`, `Get` and
   `SendCalc` can then talk to it.
2. **Calculator-to-calculator protocol.** The ESP32 listens passively between
   two Voyage 200s. A series of tests documents what they say to each other.

Treat the protocol notes as findings from these calculators, not as a
specification.

## Repository layout

```
esp32/                               Arduino sketches (one folder per sketch)
  esp32_tilink_cbl/                  experiment 1: the ESP32 as a link partner
    esp32_tilink_cbl.ino               packet layer, decoders, Serial commands
    TiLink.h, TiLink.cpp               bit-level link driver with error diagnostics
  esp32_tilink_c2c/                  experiment 2: passive sniffer between two calculators
    esp32_tilink_c2c.ino               sampler, packet printer, line history, Serial commands
    BitDecoder.h                       passive bit decoder (plain C++, testable on a PC)
  esp32_tilink_linetest/             wiring check: prints tip/ring levels without driving them
calc-to-calc-protocol-reversed/      experiment 2: documentation and data
  README.md                            sniffer wiring, flashing, running a capture session
  TESTS.md                             the test sequence (T00-T20)
  PROTOCOL.md                          the reversed calculator-to-calculator protocol
  captures/                            raw logs of every session, named by date and time
README.md                            this file; also the experiment 1 protocol notes
```

## Experiment 1: the ESP32 as a link partner

`esp32/esp32_tilink_cbl/` makes the ESP32 answer the calculator's link
protocol. Every packet is logged to the Serial Monitor (115200 baud). Type
`help` there for its commands.

- **Receives** `Send {...}` (posing as a CBL) and `SendCalc` (posing as
  another calculator). It decodes lists and strings, and shows function
  bodies as a list of tokens.
- **Answers `Get x`** with a list.
- **Pushes** string variables to the calculator unprompted (`push <name>
  <text>`). The calculator takes them silently while idle at the Home screen.

`TiLink.h` / `TiLink.cpp` implement the bit-level handshake. When a byte
fails, they report the stage, bit and line levels. What this experiment
found is under [Protocol findings](#protocol-findings-esp32--calculator)
below.

## Experiment 2: the calculator-to-calculator protocol

`esp32/esp32_tilink_c2c/` sits on the link between two Voyage 200s and only
listens. Both pins are inputs, so the calculators talk exactly as they would
over TI's cable.

- **Sampling.** One core samples the lines about every 50 ns, from IRAM with
  interrupts off, and decodes the handshake. The other core assembles
  packets, checks their checksums and prints them with timestamps.
- **Line history.** It keeps a rolling history of line changes, printed
  automatically around anything that looks wrong.
- **Probe pulses.** It recognises the calculators' 200 µs probe pulses, so
  they aren't counted as data.

Everything else for this experiment is in
[`calc-to-calc-protocol-reversed/`](calc-to-calc-protocol-reversed/):
- the sniffer's wiring (a second pigtail on the same breadboard rows);
- how to record a session;
- the test sequence;
- every capture;
- **[PROTOCOL.md](calc-to-calc-protocol-reversed/PROTOCOL.md)**, the write-up.

All tests T00–T20 have been run. They cover:
- the packet exchange for single and multiple variables;
- the RDY check that VAR-LINK uses;
- the data encoding of numbers, expressions, strings, lists, matrices,
  functions, programs, text and pictures;
- folders, overwrite prompts, aborts, `SendChat`, and a Flash app transfer.

The open questions are listed at the end of PROTOCOL.md.

## Wiring check

`esp32/esp32_tilink_linetest/` prints the tip and ring levels whenever they
change, plus a periodic heartbeat. It never drives the lines. Use it before
either experiment to check the wiring and the ground connection.

## Removed experiments

Earlier experiments were removed once the CBL sketch covered their ground.
They are in the git history, last present in commit `d4abde0`:

- `esp32/esp32_tilink_sniffer/`: an early receive-only sniffer. It took part
  in the handshake, unlike the passive c2c sniffer.
- `esp32/esp32_tilink_pingpong/` with `calculator/pingpong.c`: a raw-byte link
  using a custom frame format instead of TI's packets.
  - It needs a C program on the calculator (GCC4TI).
  - It was never tested end to end.
  - A C program could transfer faster than BASIC's `Send`/`Get`, so this
    route may be worth reviving.
- `arduino/ti_link_arduino/` with `calculator/tilink.c`: the first experiment,
  for an Arduino Uno/Nano.

## Hardware

- TI Voyage 200 (the TI-89 / TI-92 Plus family should behave the same way);
  two of them for experiment 2
- M5Stack Stamp S3 (ESP32-S3). Other ESP32 boards should work with different
  pins, except that the c2c sniffer uses ESP32-S3 features.
- 2.5 mm stereo (TRS) pigtail cables: one per calculator. The plug's moulded
  collar may need shaving down to seat fully in the Voyage 200's recessed port.
- Two 220 ohm resistors

## Wiring (Stamp S3)

The Voyage 200's link lines are open-collector with pull-ups, and on this unit
they idle at **3.3 V**, so the Stamp S3 is wired directly with no level shifter.

| Pigtail wire | Plug part | Stamp S3 |
|---|---|---|
| Red | Tip | 220 ohm -> G1 |
| White | Ring | 220 ohm -> G5 |
| Black | Sleeve | GND |

For experiment 2, the second calculator's pigtail joins the same three rows;
see [its README](calc-to-calc-protocol-reversed/README.md#wiring).

Measure your own calculator first (tip and ring against sleeve, calculator on,
at the Home screen). If the lines idle at about 5 V, use a bidirectional level
shifter.

Things learned the hard way:

- **A solid ground connection is essential.** With a loose ground the lines
  pick up hum from the laptop's power adapter and toggle every 10 ms.
- **Power order matters.** Plug in the ESP32's USB before connecting the
  calculator, and unplug the calculator first. Otherwise the calculator's
  pull-ups can half-power the unpowered ESP32 through its pin protection
  diodes, and it then fails to appear on USB.
- Stranded pigtail wires make poor breadboard contacts; tin them or solder them
  to header pins.

## Building and flashing

With `arduino-cli` and the ESP32 board package installed, from the repo root
(replace the sketch folder with the one you want):

```sh
arduino-cli compile --upload -p /dev/cu.usbmodem14301 \
  --fqbn esp32:esp32:m5stack_stamp_s3 esp32/esp32_tilink_cbl
arduino-cli monitor -p /dev/cu.usbmodem14301 -c baudrate=115200
```

Replace the port with your own (`arduino-cli board list`). The sketches also
open in the Arduino IDE.

## Protocol findings (ESP32 ↔ calculator)

From experiment 1, observed on a Voyage 200. For what two calculators say to
each other, see [PROTOCOL.md](calc-to-calc-protocol-reversed/PROTOCOL.md).

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
- **ESP32 -> calculator, push (works).** With the calculator idle at the Home
  screen, the ESP32 can send it a string variable unprompted, posing as another
  Voyage 200 (id `0x88`): VAR -> ACK, CTS -> ACK, DATA -> ACK -> EOT -> ACK, using
  the same string format as `SendCalc`. The calculator's OS only handles incoming
  transfers while idle, so its first ACK can take a few seconds. An existing
  variable of the same name is overwritten silently, without a prompt.
- **ESP32 -> calculator, `Get` (works).** `Get x` sends a REQ (`0xA2`, id `0x89`,
  data `00 00 00 00 <type> 00`) and waits. The working answer:
  ```
  calc -> REQ
  ESP  -> ACK, VAR (id 0x19, type 04, no name)   calc -> ACK, CTS
  ESP  -> ACK, DATA (count + " 7" + 00)          calc -> ACK
  ```
  The VAR header is `size (4 LE) | 04 | 00 | 00` and the DATA uses the same list
  format as `Send`. `x` then holds `{7.}`: a list, stored as decimal numbers.
  Differences from the link guide's sequence for answering a REQ, found by trial:
  - **no name** in the header; with name `FF` the calculator never ACKs it and
    then reports "Protected memory violation";
  - **no EOT** at the end; sending one leaves "Error: link transmission";
  - machine ID `0x19`; with `0x08` the calculator never ACKs the header.

  The REQ's type byte was `1E` the first time and `04` once `x` held a list, so
  it seems to be the type of the existing variable. The type in the answer's
  header (`04` or `1E`) made no difference.

### Character set

The calculator does not use Unicode. Names and text use TI's own **single-byte**
character set: one byte per character, 256 codes in total, with code 0 as the
string terminator. `0x20`-`0x7E` match ASCII, and TI's own symbols (Greek letters,
math symbols, accented letters) fill the codes from `0x80` up. Strings and variable
names use the same codes.

Symbols that look like characters on the calculator are often **tokens** instead:
`sin(`, `√(`, `∫(` and so on are stored as expression tokens and never appear in
the character set. The two tables are separate: ∑ is `8E` as a character but `BA`
as an expression token.

The `0x80`-`0x9F` block, from the published table (see Credits):

| | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **8x** | α | β | Γ | γ | Δ | δ | ε | ζ | θ | λ | ξ | ∏ | π | ρ | ∑ | σ |
| **9x** | τ | φ | ψ | Ω | ω | ᴇ | ℯ | 𝐢 | ʳ | ᵀ | x̅ | y̅ | ≤ | ≠ | ≥ | ∠ |

Captures confirm `80` α, `81` β, `83` γ, `85` δ, `86` ε (a string `"αβγδε"` arrived
as `80 81 83 85 86`) and `87` ζ (a function named ζ). The 21 Greek characters at
`80`-`94` match the 21 entries (1-9, A-L) of the calculator's Greek CHAR menu.
TI includes only some of the Greek alphabet: there is no η, ι or κ, for example.

**Remaining gap:** `0xA0`-`0xFF` is close to Latin-1 but not identical, and is not
mapped yet; the sketches print those codes as `\xNN`. To check them, store a
string containing the characters of interest, send it with `SendCalc`, and read
the codes from the hex dump.

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
- [TI calculator character sets](https://en.wikipedia.org/wiki/TI_calculator_character_sets)
  on Wikipedia for the TI-89/92/Voyage 200 character table.
