# Voyage 200 calculator-to-calculator link protocol

**Status: in progress.** T01–T03 and T01b are captured (`captures/`). Each section is filled in from the tests
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
- **In T01 there were three RDY/ACK pairs**, 3.4 s and 10.8 s apart. The VAR
  followed 24 ms after the third. Before T01, B opened VAR-LINK and then chose
  Receive; then A sent.
- **In T01b, B choosing Receive** was followed, 2.4 s later, by one RDY/ACK
  pair. A was idle at Home.
- **On A, opening VAR-LINK, selecting `a` and opening the `F3` menu** put
  nothing on the wire.
- **On A, choosing Send** gave a RDY/ACK pair, then the VAR 24 ms after the
  ACK.
- **Each RDY was answered within 0.5–0.8 ms**, even by a calculator idle at
  Home.

Inferred:
- **RDY is an "is anyone there?" check.** A calculator sends it when it
  starts a link operation: B on entering Receive, A on Send.
- **The other calculator's OS answers it**, even from the Home screen.
- That accounts for two of T01's three pairs. The third probably came from
  B opening VAR-LINK, which T01b's `mark T01b B VAR-LINK` step will check.

### 5.3 Several variables, T11; a folder, T12
### 5.4 Variable already exists: overwrite / skip / rename, T13
### 5.5 Receiver not listening, T14
### 5.6 Aborts, T15–T16
### 5.7 Large variables, T17
### 5.8 `SendChat`, T18

## 6. Variable header (VAR / RTS)

Observed (T01–T03), for `a = 5`:

```
05 00 00 00 | 00   | 01         | 61  | 00
size (LE32) | type | name length | "a" | ?
```

- **`size` (5)** counts the DATA payload after its 4 leading zero bytes: the
  2-byte length plus the 3 bytes of the value.
- **The byte after the name** was `00`. It is probably attributes (locked,
  archived): T10.

To find out:
- folder names (T09a);
- the bytes after the name: attributes for locked and archived variables
  (T10);
- name encoding (T09c, using the character set in the main README).

## 7. Data encodings

Starting point: strings and functions, as decoded in the main README, with
sizes big-endian inside the data.

| Type | Code | Test | Layout |
|---|---|---|---|
| expression / number | 00 | T01–T03, T04 | `00 00 00 00 \| length (BE16) \| content`. The integer 5 is `05 01 1F`: the value, its length in bytes (1), and the tag `1F` (positive integer). These are the same integer tokens seen in function bodies, which are read from the end backwards. |
| string | 0C | T05 | |
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
- Does opening VAR-LINK send a RDY too (T01b, first step)?
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
- How are integers longer than one byte stored (T04e)?
