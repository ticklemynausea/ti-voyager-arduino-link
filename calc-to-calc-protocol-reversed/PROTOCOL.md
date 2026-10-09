# Voyage 200 calculator-to-calculator link protocol

**Status: in progress.** T01–T05 are captured (`captures/`); T04–T05 are in
`captures/2026-10-09_190006.log`. Each section is filled in from the tests
in [TESTS.md](TESTS.md), and every finding cites the test ID that showed it.

Each section has two parts:
- **Observed:** read directly from the logs.
- **Inferred:** what the observations suggest, but haven't shown directly.

The starting points listed below come from the ESP32-to-calculator work in the
[main README](../README.md#protocol-findings). They are to be checked against
the calculator-to-calculator captures.

## 1. Physical layer and timing

Starting point:
- Two open-collector lines, idle high.
- A 0 bit is the sender pulling tip low; a 1 bit is the sender pulling ring
  low. The receiver acknowledges on the other line, and then both release.
- Bytes go LSB first.

Observed (T01–T03, all 183 bytes):
- **Bit time:** 11.6–13.4 µs from one bit to the next inside a byte.
- **Byte time:** a 4-byte packet takes 0.39–0.40 ms on the wire, so about
  100 µs per byte, or roughly 10 kB/s while a packet is being sent. Some
  packets take up to 1 ms longer, for example T02's VAR (2.45 ms against the
  usual 1.45 ms): one side paused partway through.
- **Handshake phases**, from a `trace` of the RDY and its ACK in T01b:

  | Phase | Duration |
  |---|---|
  | sender pulls its line → receiver pulls the other (ack) | 0.7–1.6 µs |
  | ack → sender lets go | 0.6–1.0 µs (once 2.0 µs) |
  | sender lets go → receiver lets go | 0.4–1.0 µs |
  | both high until the next bit | about 10 µs |

  So a bit takes about 12.5 µs, and almost all of it is the idle gap: that
  is the time the sender's code needs to fetch the next bit. **The sender
  always lets go first**, so after the both-low phase the line still low is
  the receiver's.
- **The idle gap seems to depend on the sender:** 10.0–10.6 µs in the RDY,
  9.9–10.2 µs in the ACK that answers it. One gap inside the ACK was
  19.4 µs, between two bytes. If this difference holds, it could tell the
  two calculators apart. To check with more traces.
- A passive tap must sample much faster than the bit rate to see the
  sub-microsecond phases (see the sniffer's header comment).

- **A longer packet is slower per byte:** T05c's 309-byte DATA took 38.9 ms,
  which is 126 µs per byte, or 8.1 kB/s.

To measure (T17): throughput for a large variable, and the gaps between DATA
packets.

## 2. Packet format

Starting point: `machine id | command | length (LE16)`, followed for some
commands by `data | checksum (LE16, sum of the data bytes)`.

Observed:
- VAR and DATA carry data, with a correct checksum in every capture.
- ACK, CTS, EOT and RDY are header only.
- **The length field of a header-only packet isn't always 0.** The ACK that
  answers RDY has length `0x100C` (4108), with no data following (T01).

To confirm: the maximum DATA size (T17), and what `0x100C` means.

## 3. Machine IDs

Starting point: `SendCalc` from a Voyage 200 uses `0x88`, and expects replies
with `0x88` as well.

Observed (T01–T03):
- **Every transfer packet uses `0x88`**, from both calculators: VAR, CTS, DATA,
  EOT and every ACK.
- **The RDY check and its ACK use `0x89`**, both ways (T01). That is the same
  ID a Voyage 200 uses for the CBL-style `Send` (main README).

Inferred: since both sides use the same ID, a packet's direction can't be
read from the ID. It follows from the command order: the side that sends VAR
also sends DATA and EOT, and the other side sends CTS. ACKs alternate between
the two.

## 4. Commands

| Code | Name | Data | Seen in |
|---|---|---|---|
| 06 | VAR | variable header | T01–T03 |
| 09 | CTS | — | T01–T03 |
| 15 | DATA | variable contents | T01–T03 |
| 36 | SKIP | reason code | |
| 56 | ACK | — (length field `0x100C` when answering RDY) | T01–T03 |
| 5A | ERR | — | |
| 68 | RDY | — | T01 |
| 92 | EOT | — | T01–T03 |
| A2 | REQ | variable header | |
| C9 | RTS | variable header | |

## 5. Sequences

One diagram per scenario, with A (sender) on the left and B on the right.

### 5.1 Single variable: one exchange for all three methods (T01–T03)

VAR-LINK Send, `SendCalc` to a calculator idle at Home, and `SendCalc` to a
calculator waiting in `GetCalc` all produce **the same packets with the same
bytes**. The only difference is that VAR-LINK checks first with RDY (5.2):

```
A (sender)                         B (receiver)
VAR   88 06 08 00 <header> <ck>  ->
                                 <-  ACK  88 56 00 00
                                 <-  CTS  88 09 00 00
ACK   88 56 00 00                ->
DATA  88 15 09 00 <data> <ck>    ->
                                 <-  ACK  88 56 00 00
EOT   88 92 00 00                ->
                                 <-  ACK  88 56 00 00
```

Observed:
- **The receiver's ACK and CTS come as a pair**, about 60 µs apart.
- **The sender's ACK of the CTS** follows 0.6–0.9 ms later, and DATA about
  150 µs after that.
- **How quickly B answers depends on what B is doing:**

  | B is… | VAR → ACK | EOT → ACK |
  |---|---|---|
  | in VAR-LINK Receive (T01) | 1.5 ms | 24 ms |
  | idle at Home (T02) | 40 ms | 14 ms |
  | waiting in `GetCalc` (T03) | 1.5 ms | 4.9 ms |

- **`GetCalc` starts nothing.** In T03, B sent no request (no REQ). It waited
  for A's VAR, and the exchange is identical to T02. A calculator at Home
  accepts the same transfer silently.

### 5.2 The RDY check before a VAR-LINK send (T01)

```
RDY   89 68 00 00                ->
                                 <-  ACK  89 56 0C 10   (length field 0x100C, no data)
```

Observed:
- **T01:** three RDY/ACK pairs, 3.4 s and 10.8 s apart, with the VAR 24 ms
  after the third. Beforehand, B opened VAR-LINK and chose Receive, then A
  sent.
- **T01b rerun, a mark before every step** (`captures/2026-10-09_182735.log`):

  | Step | On the wire |
  |---|---|
  | B opens VAR-LINK | one RDY/ACK pair, 2.9 s after the mark |
  | B chooses Receive | nothing |
  | A opens VAR-LINK, selects `a`, opens `F3` | nothing |
  | A chooses Send | RDY/ACK, then VAR 24 ms after the ACK |

  The sniffer missed the first two bits of the first pair: it shows up as
  `22 1A 00 40 A2 15 03` plus 6 bits. That is exactly `89 68 00 00 89 56 0C 10`
  shifted by two bits. The cause was a sniffer bug, fixed in the next sketch
  version (see its header comment).
- **The first T01b run** saw one RDY/ACK pair 2.4 s after `mark T01b B
  Receive`, while B was opening VAR-LINK and choosing Receive. That fits the
  rerun: the RDY came from opening VAR-LINK.
- **Each RDY was answered within 0.5–0.8 ms**, even by a calculator idle at
  Home.

Inferred:
- **RDY is an "is anyone there?" check.** A calculator sends it when VAR-LINK
  opens and again when it sends. The other calculator's OS answers it, even
  from the Home screen.
- **Choosing Receive sends nothing**: the receiver just waits.
- **T04 and T05 fit this.** In T04, VAR-LINK was reopened for each test, and
  each test had two RDY/ACK pairs, 11–17 s apart. In T05, VAR-LINK stayed open
  (Receive was chosen again each time), and each test had one pair, at Send.
- **T01's third RDY is not yet explained.** A opening VAR-LINK sent nothing in
  the rerun. Maybe B opened VAR-LINK twice in T01.

### 5.3 Several variables (seen in the first T05a attempt; T11 to confirm)

Some variables were still selected in VAR-LINK from T04, so the first T05a
attempt sent `a` and `s0` together:

```
A: RDY  ->  B: ACK
A: VAR a          ->  B: ACK, CTS  ->  A: ACK, DATA  ->  B: ACK
A: VAR s0 (30 ms later)  ->  B: ACK, CTS  ->  A: ACK, DATA  ->  B: ACK
A: EOT            ->  B: ACK
```

Observed:
- **There is no EOT between the variables.** The next VAR simply follows,
  about 30 ms after the previous DATA's ACK.
- **One EOT ends the whole batch.**

A second attempt, sending `a` and `s1`, lost the first byte of its VAR
(sniffer bug). What was left of it shows the same pattern, with a 2.5 s pause
in the middle, perhaps while B asked whether to overwrite `a`. T13 will
capture that cleanly.

### 5.4 Folders, T12
### 5.5 Variable already exists: overwrite / skip / rename, T13
### 5.6 Receiver not listening, T14
### 5.7 Aborts, T15–T16
### 5.8 Large variables, T17
### 5.9 `SendChat`, T18

## 6. Variable header (VAR / RTS)

Observed (T01–T03), for `a = 5`:

```
05 00 00 00 | 00   | 01         | 61  | 00
size (LE32) | type | name length | "a" | ?
```

- **`size` (5)** counts the DATA payload after its 4 leading zero bytes: the
  2-byte length plus the 3 bytes of the value. This holds for every variable
  in T04–T05, for example 305 for T05c's 309-byte DATA.
- **`size` is little-endian:** 305 is sent as `31 01 00 00`.
- **The type** was `00` for every T04 value (numbers, fractions, complex
  numbers, `x+1`) and `0C` for strings.
- **The byte after the name** was `00`. It is probably attributes (locked,
  archived): T10.

To find out:
- folder names (T09a);
- the bytes after the name: attributes for locked and archived variables
  (T10);
- name encoding (T09c, using the character set in the main README).

## 7. Data encodings

Every DATA payload is laid out the same way:

```
00 00 00 00 | length (BE16) | content
```

- **`length` is big-endian:** 303 is sent as `01 2F` (T05c).
- **The content is in the calculator's expression format:** a sequence of
  tags that is read **from the end backwards**. The last byte is the outermost
  tag, and each tag's operands come before it.

Tags observed so far (T01–T05, plus the function captures in the main
README):

| Tag | Meaning | Operands, reading backwards from the tag | Seen in |
|---|---|---|---|
| `1F` | positive integer | length *n*, then *n* bytes of value, **little-endian** | T01, T04e |
| `20` | negative integer | as `1F` | T04a |
| `21` | positive fraction | numerator, then denominator, each as length + bytes | T04c |
| `23` | float | 9 bytes: exponent (BE16, bias `0x4000`), then 14 BCD digits | T04b, T04d |
| `26` | 𝐢, probably | none | T04f |
| `8B` | + | two operands | T04f, T04g |
| `8F` | × | two operands | T04f |
| `08` | variable x | none | T04g |
| `2D` | string | `00`, the text, `00` (reading forwards) | T05 |

These values match the expression tags in the TIGCC documentation (`estack.h`,
for example `POSINT_TAG` 1F, `FLOAT_TAG` 23, `ADD_TAG` 8B, `MUL_TAG` 8F,
`STR_DATA_TAG` 2D, `END_TAG` E5). That list is the reference for decoding
the rest; the captures here confirm the entries one at a time.

Worked examples (content only, after the length):

| Test | Value | Content | Reading |
|---|---|---|---|
| T01 | 5 | `05 01 1F` | integer, 1 byte, 5 |
| T04a | −5 | `05 01 20` | negative integer, 1 byte, 5 |
| T04b | 1.5 | `40 00 15 00 00 00 00 00 00 23` | float: exponent `4000` = 10⁰, digits 1.5000… |
| T04c | 1/3 | `03 01 01 01 21` | fraction: numerator `01 01` = 1, denominator `03 01` = 3 |
| T04d | 1.1E100 | `40 64 11 00 00 00 00 00 00 23` | float: exponent `4064` = 10¹⁰⁰, digits 1.1 |
| T04e | 2⁷⁰ | `00 00 00 00 00 00 00 00 40 09 1F` | integer, 9 bytes, little-endian: `40` in the top byte = 2⁶·2⁶⁴ |
| T04f | 2+3𝐢 | `02 01 1F 03 01 1F 26 8F 8B` | + ( × (𝐢, 3), 2 ): stored as an expression, not as a complex-number type |
| T04g | x+1 | `08 01 01 1F 8B` | + (1, x) |
| T05a | `""` | `00 00 2D` | empty string |
| T05b | `"a"` | `00 61 00 2D` | one character |
| T05c | 300 digits | `00 "1000…0" 00 2D` | length `01 2F` = 303 |

- **T04d** was entered as `1.1E100` (the test plan said `1.E100`), which
  matches the digits.
- **T05b's** string was entered as lowercase `"a"`.

| Type | Code | Test | Layout |
|---|---|---|---|
| expression / number | 00 | T01–T04 | expression tags, see above |
| string | 0C | T05 | `00 \| text \| 00 \| 2D` |
| list | 04 | T06a–b | |
| matrix | 06 | T06c | |
| function | 13 | T07a | |
| program | 12 | T07b | |
| text | 0B | T08a | |
| picture | 10 | T08b | |
| data | 0A | T08c | |
| GDB | 0D | T08d | |

## 8. Open questions

- What does the ACK length field `0x100C` mean in the answer to RDY (5.2)?
- Why did A opening VAR-LINK send nothing, when B opening it sent a RDY? Is
  it the first time VAR-LINK opens, or does it depend on the other calculator?
- **Unexplained pulse groups (T01b trace, before the mark, while B was in
  VAR-LINK):**
  - One calculator pulls tip and gets an acknowledgement on ring within
    0.9 µs, but then holds tip for 201 µs before letting go, instead of
    about 1 µs. The ring is released 0.4 µs later.
  - The pulses come in groups of three, about 16.5 ms apart, and the groups
    are seconds apart.
  - The sniffer decodes each pulse as a 0 bit, and so would report a partial
    byte.

  The pattern looks like a presence or activity probe rather than data. To
  pin down when it happens, mark each step.
