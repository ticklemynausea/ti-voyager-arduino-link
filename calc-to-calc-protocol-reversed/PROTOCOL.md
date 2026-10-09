# Voyage 200 calculator-to-calculator link protocol

**Status: all tests captured; some questions open (section 8).** The logs are
in `captures/`:
- T04–T05: `2026-10-09_190006.log`;
- T06–T12: `2026-10-09_192238.log`;
- T13–T20: `2026-10-09_201528.log`. That log has a test marked "T13" between
  T13c and T15, which is taken here to be T14.

From T06 on, variables were sent with `SendCalc` to B idle at Home, except
T09a's first run, T11 and T12, which used VAR-LINK. Each section is filled in from the tests
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
  answers RDY has length `0x100C` (4108), or sometimes `0x110C`, with no data
  following (T01; section 8).
- **A variable's data goes in a single DATA packet.** The biggest seen was
  10008 bytes (T17), sent in 1.04 s at 9.6 kB/s. Only Flash apps are split
  into several packets (5.10).
- **CONT (`78`)** is header only (T20).

## 3. Machine IDs

Starting point: `SendCalc` from a Voyage 200 uses `0x88`, and expects replies
with `0x88` as well.

Observed (T01–T03):
- **Every transfer packet uses `0x88`**, from both calculators: VAR, CTS, DATA,
  EOT and every ACK.
- **The RDY check and its ACK use `0x89`**, both ways (T01). That is the same
  ID a Voyage 200 uses for the CBL-style `Send` (main README).

- **`SendChat` uses `0x89`** for every packet of its transfer (T18).

Inferred: since both sides use the same ID, a packet's direction can't be
read from the ID. It follows from the command order: the side that sends VAR
also sends DATA and EOT, and the other side sends CTS. ACKs alternate between
the two.

## 4. Commands

| Code | Name | Data | Seen in |
|---|---|---|---|
| 06 | VAR | variable header | all transfers |
| 09 | CTS | — | all transfers |
| 15 | DATA | variable contents | all transfers |
| 36 | SKIP | reason code | not seen, not even for "No" to an overwrite (T13b) |
| 56 | ACK | — (length field `0x100C` or `0x110C` when answering RDY) | all transfers |
| 5A | ERR | — | not seen |
| 68 | RDY | — | VAR-LINK |
| 78 | CONT | — | Flash app transfer (T20) |
| 92 | EOT | — | all transfers |
| A2 | REQ | variable header | not seen between calculators |
| C9 | RTS | variable header | not seen between calculators |

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

- **Transfers are deterministic (T19):** two identical sends gave
  byte-identical packets. Only the timing differed.
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
  shifted by two bits. The cause was a sniffer bug: at start-up the decoder
  ignored the first bit when the lines were already idle, and this was the
  session's first traffic. It happened again in `captures/2026-10-09_192238.log`,
  where the automatic line history shows the lost bit was on the wire. Fixed
  since.
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

### 5.3 Several variables (T11, and the first T05a attempt)

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

**T11** (`a`, `l1` and `s1` selected together) confirms it: three VARs in a
row, each 22 ms after the previous DATA's ACK, and one EOT at the end.

**An overwrite prompt** (T11: B already had `s1`, and asked):
- **B holds back its ACK to the VAR** until the user answers. The ACK came
  2.99 s after the VAR, followed as usual by CTS.
- **The prompt adds no packets of its own.**
- B's final ACK to the EOT also took longer than usual: 0.6 s.

A second T05a attempt, sending `a` and `s1`, lost the first byte of its VAR
(sniffer bug). What was left of it shows the same 2.5 s pause, consistent with
an overwrite prompt for `a`.

### 5.4 Folders (T09a, T12)

- **Sending a variable from inside folder `tst` (T09a)** sends only its name:
  `b`. The receiver files it in its own current folder, presumably.
  - Both VAR-LINK and `SendCalc` do this.
- **Sending the folder itself (T12)** sends each variable with its folder in
  the name: `tst\b`, in an ordinary VAR, with `5C` (`\`) as the separator.
  - There is no separate packet for the folder.
  - The folder held only `b`, so it's not yet known how several variables in
    a folder are sent.

### 5.5 Variable already exists (T13, T17, T19)

**In VAR-LINK Receive, B asks after the data has arrived.** The transfer runs
as usual; B then holds back its ACK to the final EOT until the user answers:

| Test | Answer | After A's EOT |
|---|---|---|
| T13a | Yes (overwrite) | B's ACK after 3.23 s |
| T17, T19-2 | Yes | B's ACK after 2.31 s and 2.18 s |
| T13b | No | **no ACK at all** |
| T13c | Rename offered but disabled | no ACK |

Observed:
- **No SKIP (`36`) packet is ever sent.** Refusing the variable just means
  the EOT is never acknowledged.
- **In T11 (several variables), the delay came at the third variable's VAR**
  instead (2.99 s before B's ACK), with the final EOT acknowledged after
  0.6 s.

Inferred: A has no way to tell "No" apart from a lost link, other than by
timing out. (What did A show in T13b and T13c?)

### 5.6 Receiver busy (T14: B in the MODE dialog)

```
A: RDY  ->  B: ACK (0x110C)
A: VAR  ->  (nothing)
```

- **B never acknowledges the VAR.** A shows "ERROR: Link Transmission".
- **The only other activity is probe pulses** (5.11), 22 ms and 1.8 s after
  the VAR.
- **B's OS still answers RDY** while in the dialog.

### 5.7 Aborts (T15, T16)

Both tests sent the 10 KB list `big` and pressed ON partway through.

- **ON on A, the sender (T15):** the DATA packet still completed, all 10008
  bytes with a correct checksum, in 1.17 s against the usual 1.04 s. Then
  nothing more: B never acknowledged the DATA, and A sent no EOT. Only probe
  pulses followed.
- **ON on B, the receiver (T16):** the DATA packet also completed, but took
  1.82 s.
  - Over the second half of the packet, seven extra pulses were mixed in
    between the data bits. The old decoder counted each as a 0 bit, which
    shifted the data and broke the checksum.
  - With those seven bits removed, the packet matches T17's byte for byte,
    checksum included.
  - After the DATA, again nothing but probe pulses.

Inferred:
- **Neither calculator stops mid-packet.** The ON key is only acted on
  between packets.
- **An abort is silent:** no ERR or SKIP, the other side just gets no
  further packets.
- **The pulses during T16** look like the probes in 5.11, made while B
  handled the ON key. The decoder now classifies such pulses as probes, but
  these seven had already scrolled out of the line history, so that is not
  confirmed for them.

### 5.8 Large variables (T17)

The 10 KB list went as one 10008-byte DATA packet, in 1.04 s (9.6 kB/s), and
decoded correctly. Its elements are floats such as
`40 02 14 28 57 14 28 57 14 23` (142.85714285714) and
`3F FF 14 28 57 14 28 57 14 23` (0.14285714285714): exponents below 10⁰ go
under the bias `0x4000`.

### 5.9 `SendChat` (T18: B in `GetCalc`, A `SendChat a`)

The same packets as `SendCalc`, byte for byte, except that **every packet
carries machine ID `0x89` instead of `0x88`**, on both sides. There was no
RDY.

### 5.10 Flash application (T20: B sends the app "TIESP", Español, to A)

```
B: VAR (type 24, name "TIESP", size 39907)   ->  A: ACK
                                             <-  A: CTS        (1.35 s later)
B: ACK, DATA (7016 bytes)                    ->  A: ACK
B: CONT                                      ->  A: ACK, CTS
B: ACK, DATA (4976 bytes)                    ->  A: ACK
   ... CONT / ACK, CTS / ACK, DATA / ACK, for each part ...
B: EOT                                       ->  A: ACK        (2.2 s later)
?: EOT                                       ->  ?: ACK        (10 ms later)
```

Observed:
- **The app is sent in 12 DATA packets** of 7016, 4976, 1022, 1020 (seven
  times), 660, 17914, 988 and 191 bytes. Together that is exactly 39907
  bytes, the size in the VAR header.
- **Unlike variable data, these packets have no `00 00 00 00 | length`
  prefix.** They are the app's raw bytes.
  - The first starts with TI's Flash header (`81 0E … 81 45 "TIESP" …`).
  - The 17914-byte part holds the Spanish strings ("DD/MM/YYYY", "Español",
    "¿1º Vértice?").
- **Each further part is announced by CONT (`78`)**, presumably from the
  sender, and answered with ACK and CTS.
- **The VAR header's byte after the name** is `03`, as for the GDB.
- **A answered the VAR immediately, but took 1.35 s to send CTS.** It took
  2.2 s to acknowledge the end: probably erasing and writing Flash.
- **The transfer ends with two EOT/ACK pairs, 10 ms apart.** Which side sends
  the second EOT isn't known.
- **Throughput was 9.0 kB/s per packet,** and the whole app took about 9 s.

**A false start** before T20 shows a send with nothing in it: RDY/ACK,
another RDY/ACK, then a lone EOT and its ACK.

### 5.11 Probe pulses

Observed in nearly every session:
- **About 16 ms after a transfer ends,** and again repeatedly afterwards.
- **While a transfer is held up** (5.6, 5.7).

The waveform, from the line history (T13–T19):
1. One calculator pulls tip.
2. The other acknowledges on ring within about 1 µs.
3. The first holds tip for 193–202 µs, then lets go.
4. The ring is let go 0.5 µs later.

That is like a single 0 bit, but the both-low phase lasts about 200 µs
instead of about 1 µs. The pulses come in threes, about 16.5 ms apart, and
the groups repeat every 4.47 s.

Inferred:
- **These pulses are not data:** no byte ever follows them.
- **They look like a calculator's OS checking, in software, whether the other
  side is there.** The 1 µs answers to every bit, and the 200 µs holds here,
  suggest the normal bit handshake is done by the link hardware.
- **The sniffer now reports them as `probe` lines** and doesn't count them as
  bits. The old decoder reported a probe on its own as a 1-bit partial byte.

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
- **The name** is in TI's character set. `λ` is sent as `89` (T09c), and an
  8-character name works (T09b). A name sent with its folder uses `\`:
  `tst\b` (T12).
- **The byte after the name is not the lock or archive state.** It was `00`
  for the locked `lk` and the archived `ar` (T10a–b), just as for ordinary
  variables. It was:

  | Value | Seen for |
  |---|---|
  | `00` | numbers, strings, lists, matrices, programs, text, pictures, data |
  | `01` | functions (T07a, and the main README's captures) |
  | `03` | the GDB `g1` (T08d) and the Flash app (T20) |

  Its meaning is unknown.
- **Locked and archived variables** arrive like any other. Whether B keeps
  them locked or archived wasn't checked.

## 7. Data encodings

Every DATA payload is laid out the same way:

```
00 00 00 00 | length (BE16) | content
```

- **`length` is big-endian:** 303 is sent as `01 2F` (T05c).
- **The content is in the calculator's expression format:** a sequence of
  tags that is read **from the end backwards**. The last byte is the outermost
  tag, and each tag's operands come before it.

Tags observed so far (T01–T08, plus the function captures in the main
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
| `D9` | list | elements, first element nearest the tag, closed by `E5` | T06 |
| `E5` | end of a list or of arguments | none | T06, T07 |
| `DC` | function or program | see below | T07 |
| `E0` | text file | see below | T08a |
| `DF` | picture | see below | T08b |
| `DD` | data variable | see below | T08c |
| `DE` | GDB | see below | T08d |

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
| T06a | {1,2,3} | `E5 03 01 1F 02 01 1F 01 01 1F D9` | list: 1, 2, 3, end |
| T06b | {"a","b"} | `E5 00 62 00 2D 00 61 00 2D D9` | list: "a", "b", end |
| T06c | [[1,2][3,4]] | `E5 E5 04 01 1F 03 01 1F D9 E5 02 01 1F 01 01 1F D9 D9` | a list of row lists: {1,2}, {3,4}. The VAR type (`06`) is what makes it a matrix |

- **T04d** was entered as `1.1E100` (the test plan said `1.E100`), which
  matches the digits.
- **T05b's** string was entered as lowercase `"a"`.

**Functions and programs (T07):**

```
E9 | body | E5 | parameters | 00 | ?? | 40 | DC      (reading forwards)
```

- **`f(x)=x^2` (T07a):** body `02 01 1F 08 F0 93` (x^2, with `08 F0` meaning
  the parameter x), parameters `08` (x), then `00 00 40 DC`.
- **`p()=Prgm:Disp "hi":EndPrgm` (T07b):** body
  `12 E4 00 E7 E5 00 68 69 00 2D 7A E4 00 E7 19 E4`, no parameters, then
  `00 00 40 DC`.
- **Inferred, reading backwards:**
  - `19 E4` is one command (Prgm), with `E4` the command tag and the byte
    before it the command number;
  - `00 E7` ends a statement;
  - `7A E4`, preceded by `E5 "hi"`, is `Disp "hi"`;
  - `12 E4` is EndPrgm.

  Note that `7A` is a command number here, but was negation in an
  expression (main README): the meaning of a byte depends on the tag around
  it.

**Text (T08a):** `00 06 | 20 "hello" 00 | E0`.
- `00 06` is probably the cursor position: 6 is the end of the text.
- Each line starts with a one-character mark (a space here), as in the text
  editor.

**Picture (T08b):** `00 67 | 00 EF | rows | DF`.
- That is 103 rows by 239 columns: the full graph screen.
- Each row is 30 bytes, one bit per pixel. 103 × 30 + 4 + 1 = 3095, the DATA
  length.
- The 3101-byte DATA went in a single packet, at 9.6 kB/s.

**Data variable (T08c), `NewData d1,{1,2},{3,4}`:**
`04 02 01 00 08 E5 {1,2} D9 02 00 08 E5 {3,4} D9 00 00 DD`.
- The two columns are ordinary lists.
- The bytes around them (`04 02 01 00 08`, `02 00 08`, `00 00`) are not
  decoded yet.

**GDB (T08d):** 236 bytes ending in `D9 DE`, with floats (`23`) inside. Not
decoded yet.

| Type | Code | Test | Layout |
|---|---|---|---|
| expression / number | 00 | T01–T04 | expression tags, see above |
| string | 0C | T05 | `00 \| text \| 00 \| 2D` |
| list | 04 | T06a–b | `E5 \| elements \| D9` |
| matrix | 06 | T06c | a list of row lists |
| function | 13 | T07a | `E9 \| body \| E5 \| params \| 00 \| ?? \| 40 \| DC` |
| program | 12 | T07b | as a function, with commands (`E4`) and statement ends (`E7`) |
| text | 0B | T08a | `cursor (BE16) \| lines \| 00 \| E0` |
| picture | 10 | T08b | `rows (BE16) \| columns (BE16) \| bitmap \| DF` |
| data | 0A | T08c | column lists plus undecoded bytes, `DD` |
| GDB | 0D | T08d | undecoded, ends `D9 DE` |

## 8. Open questions

- **What does the ACK length field mean in the answer to RDY (5.2)?**
  - It is usually `0x100C`, but sometimes `0x110C`, which differs in one bit.
  - `0x110C` was seen: after the start-up check at 135.8 s; for the two RDYs
    just after T09a's VAR-LINK transfer; and for T11's first RDY, all in
    `captures/2026-10-09_192238.log`. Also for the first RDY of T14 and of
    T15, and for the second RDY of T20's false start, in
    `captures/2026-10-09_201528.log`.
  - Perhaps it reflects the answering calculator's state, or tells which
    calculator answered.
- Which calculator makes the probe pulses, and what for (5.11)? They are the
  same as the pulse groups first seen in the T01b trace.
- In T20, which side sends the second EOT, and which sends CONT?
- What did A show after B answered "No" (T13b), and after T13c?
- What do the unknown bytes in functions (`00 ?? 40` before `DC`), data
  variables and GDBs mean?
- Why did A opening VAR-LINK send nothing, when B opening it sent a RDY? Is
  it the first time VAR-LINK opens, or does it depend on the other calculator?
