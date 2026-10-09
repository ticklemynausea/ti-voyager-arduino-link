# Voyage 200 calculator-to-calculator link protocol

**Status: skeleton.** Nothing in this file has been confirmed by
calculator-to-calculator captures yet. Each section is filled in from the
tests in [TESTS.md](TESTS.md), and every finding cites the test ID and the log
in `captures/` that showed it.

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

To measure (T17, `stats`): the bit time, the byte time, the gap between bytes
and between packets, and throughput.

## 2. Packet format

Starting point: `machine id | command | length (LE16)`, followed for some
commands by `data | checksum (LE16, sum of the data bytes)`.

To confirm:
- which commands carry data;
- whether the length field of header-only packets is ever non-zero, and what
  it means then;
- the maximum DATA size (T17).

## 3. Machine IDs

Starting point: `SendCalc` from a Voyage 200 uses `0x88`, and expects replies
with `0x88` as well.

To find out:
- whether both calculators use `0x88`;
- whether VAR-LINK uses different IDs from `SendCalc`;
- how the direction of a packet can be told when both sides share an ID.

## 4. Commands

| Code | Name | Data | Seen in |
|---|---|---|---|
| 06 | VAR | variable header | |
| 09 | CTS | — | |
| 15 | DATA | variable contents | |
| 36 | SKIP | reason code | |
| 56 | ACK | — | |
| 5A | ERR | — | |
| 68 | RDY | — | |
| 92 | EOT | — | |
| A2 | REQ | variable header | |
| C9 | RTS | variable header | |

## 5. Sequences

One diagram per scenario, with A (sender) on the left and B on the right.

### 5.1 Single variable (VAR-LINK), T01
### 5.2 Silent transfer (`SendCalc` to an idle calculator), T02
### 5.3 `GetCalc` / `SendCalc`, T03
### 5.4 Several variables, T11; a folder, T12
### 5.5 Variable already exists: overwrite / skip / rename, T13
### 5.6 Receiver not listening, T14
### 5.7 Aborts, T15–T16
### 5.8 Large variables, T17
### 5.9 `SendChat`, T18

## 6. Variable header (VAR / RTS)

Starting point: `size (LE32) | type | name length | name | …`

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
| expression / number | 00 | T04 | |
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
