# Test sequence

Each test changes **one thing**, so differences between captures can be
pinned on it. **A** is the sending calculator and **B** the receiving one,
unless the test says otherwise.

How to run each test:
- Type `mark <ID>` in the monitor first.
- Do the steps.
- Note anything the calculators show (prompts, errors) by typing it as a mark
  too, e.g. `mark T13a B asked: Overwrite? -> Yes`.

Keys on the Voyage 200:
- **VAR-LINK** is `2nd` `−`.
- In VAR-LINK, select a variable with `F4` (✓). `F3` Link → *Send to
  TI-89/92 Plus* sends it, and `F3` Link → *Receive* waits for one.

## Before you start

1. **Back up** anything important on both calculators. Transfers can overwrite
   variables silently.
2. **Never** use *Send Product SW* / OS transfer between them.
3. On **A**, at the Home screen, create the test variables. Mode: leave
   *Exact/Approx* on AUTO.

```
5→a
"a"→s1
""→s0
string(10^299)→s3
{1,2,3}→l1
{"a","b"}→l2
[[1,2][3,4]]→m1
Define f(x)=x^2
Define p()=Prgm:Disp "hi":EndPrgm
NewData d1,{1,2},{3,4}
StoGDB g1
StoPic pic1
9→abcdefgh
4→λ
6→lk
Lock lk
8→ar
Archive ar
seq(i/7.,i,1,1000)→big
NewFold tst
7→b
setFold(main)
```

Notes on these lines:
- `s3` is a 300-character string of digits.
- `big` is about 10 KB.
- `NewFold` makes `tst` the current folder, so `b` lands in it. `setFold(main)`
  goes back.

For **T08a**, make a text file on A: `APPS` → Text Editor → New, name `t1`, type
`hello`, then go back to Home.

4. On **B**, delete any variables with these names (VAR-LINK, `F1` → Delete),
   so transfers aren't affected by name clashes until T13.

## Validation (send me this log first)

| ID | Do | Shows |
|---|---|---|
| T00 | Monitor running, both calculators off. Switch A on, then B. Wait 10 s. Switch A off and on. Run `stats`. | noise, power-on chatter; ambiguous bits must be 0 |
| T01 | B: VAR-LINK → `F3` Receive. A: VAR-LINK, ✓ `a`, `F3` → Send. | the basic sequence, machine IDs, the VAR header |
| T01b | Repeat T01 with a mark before each step: `mark T01b B VAR-LINK`, `mark T01b B Receive`, `mark T01b A VAR-LINK`, `mark T01b A select a`, `mark T01b A F3`, `mark T01b A Send`. Wait a few seconds between steps. | which actions send RDY, and from which calculator |
| T02 | B idle at Home. A at Home: `SendCalc a` | silent transfer vs VAR-LINK |
| T03 | B at Home: `GetCalc a` (it waits). A: `SendCalc a` | GetCalc's side; whether the receiver initiates |

## Encodings

Use T01's method (VAR-LINK Receive on B, Send on A), unless noted. For T04, set
`a` on A before each sub-test, and delete `a` on B between sub-tests (or answer
Yes to overwrite, and note it).

| ID | Variable | Shows |
|---|---|---|
| T04a | `-5→a` | negative integer |
| T04b | `1.5→a` | float (BCD?) |
| T04c | `1/3→a` | exact fraction |
| T04d | `1.1E100→a` | exponent |
| T04e | `2^70→a` | big integer |
| T04f | `2+3𝐢→a` | complex |
| T04g | `x+1→a` (make sure `x` is undefined: `DelVar x`) | symbolic expression |
| T05a | `s0` | empty string |
| T05b | `s1` | one-character string |
| T05c | `s3` | 300 characters: lengths past 255, byte order |
| T06a | `l1` | list of numbers |
| T06b | `l2` | list of strings |
| T06c | `m1` | matrix |
| T07a | `f` | function |
| T07b | `p` | program |
| T08a | `t1` | text |
| T08b | `pic1` | picture |
| T08c | `d1` | data |
| T08d | `g1` | graph database |

## Names, folders, attributes

| ID | Do | Shows |
|---|---|---|
| T09a | send `tst\b` (in VAR-LINK, open folder `tst`) | how the folder is named in the header |
| T09b | send `abcdefgh` | 8-character name |
| T09c | send `λ` | non-ASCII name |
| T10a | send `lk` (locked) | attribute byte |
| T10b | send `ar` (archived) | attribute byte |
| T11 | ✓ `a`, `s1`, `l1` together, send | multi-variable flow: one VAR per variable? EOT where? |
| T12 | ✓ folder `tst` itself, send | folder transfer |

## Conflicts and errors

| ID | Do | Shows |
|---|---|---|
| T13a | B already has `a`. Send `a`; on B answer **Yes** (overwrite) | overwrite flow |
| T13b | same, answer **No** | SKIP packet and its reason code |
| T13c | same, choose **Rename** if offered, new name `a2` | rename flow |
| T14 | B in the MODE dialog (not Receive, not Home). A sends `a` | refusal or timeout |
| T15 | Send `big`; press `ON` on **A** halfway | sender abort |
| T16 | Send `big`; press `ON` on **B** halfway | receiver abort |
| T17 | Send `big`, let it finish | data packet sizes, throughput |

## Other

| ID | Do | Shows |
|---|---|---|
| T18 | B at Home: `GetCalc a`. A: `SendChat a` (skip if `SendChat` doesn't exist) | chat variant of SendCalc |
| T19 | Repeat T01 twice (`mark T19-1`, `mark T19-2`) | what changes between identical runs |
| T20 | Optional: VAR-LINK `F7` FlashApps on A, send a small app B doesn't have | Flash app transfer (long; skip if unsure) |

Finish with `stats` and send me the log from `captures/`.
