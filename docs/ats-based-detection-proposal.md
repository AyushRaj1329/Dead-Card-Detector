# Proposal: Detect card "aliveness" via ATS instead of UID

## Background

The current firmware (`dead_card_detector.ino`) determines whether a card
is "alive" by attempting to read its **UID** through the RRHFOEM04
reader. Testing surfaced two separate problems with this approach:

1. **Some cards fail to return a UID inconsistently** even when
   physically present on the reader, traced to physical/RF factors
   (card positioning, wallet interference with nearby cards, possible
   antenna wear) rather than a firmware bug — see
   `docs/troubleshooting-uid-read.md` for the full investigation.
2. **The RRHFOEM04 reader itself has a hard protocol ceiling.** Its
   command set — confirmed directly from RapidRadio's own RRHFOEM04 CS
   test application source code — only implements **ISO14443-3**
   (Request, Wake up, Anti-collision, Select Card, Inventory, Halt).
   It has no RATS/ATS command at all. This isn't a bug to fix; the
   reader hardware/firmware simply does not go further than the UID
   layer, for any card.

This raised the question: would checking for **ATS** (Answer To Select)
instead of UID be a better/more reliable signal of a card being "alive,"
and what would it take to implement?

## What ATS actually is

- **UID** (Unique Identifier) is returned during ISO14443-3
  Anti-collision. It's just the card's serial number — no
  cryptography, no application-level exchange. Getting a UID confirms
  the card powered up and responded to basic RF addressing, nothing
  more.
- **ATS** (Answer To Select) is returned in response to a **RATS**
  (Request for Answer To Select) command, which is the entry point to
  **ISO14443-4** (the "T=CL" transport protocol layer). ATS tells the
  reader the card's supported frame size, timing parameters, and
  protocol capabilities so that higher-level APDU exchange (the kind
  EMV payment cards and DESFire cards use) can begin.
- Practically: **UID confirms an ISO14443-3 card is present. ATS
  confirms the card additionally supports ISO14443-4** — a stronger,
  deeper handshake than UID alone. Not every 13.56 MHz card supports
  ISO14443-4 (e.g. plain MIFARE Classic/Ultralight cards, common on
  transit/metro cards, typically do not — they stay at the
  ISO14443-3 layer), so this is not a strict superset that works for
  every card type; it's a different signal that happens to be more
  informative for cards that do support it (most EMV bank cards do).

## Why the current reader (RRHFOEM04) can't do this

Confirmed by inspecting `RRHFOEM04 CS v1.4/RRHFOEM04/frmMain.cs`
(the manufacturer's own reference application source, included in this
repo's `RRHFOEM04-TCP/` folder) — there is no RATS/ATS command
anywhere in its command set or protocol. This is a hardware/firmware
limitation of the reader itself, not something fixable by changing
`dead_card_detector.ino`. Getting ATS requires different reader
hardware.

## Reader hardware options for ATS

### Option A: PN532 (recommended)

- NXP PN532 NFC/RFID front-end chip, widely available as Arduino-ready
  breakout modules (Adafruit, Elechouse, generic clones).
- **Connects directly to Arduino Uno R3** via SPI, I2C, or UART — no
  separate PC/host needed, keeping this project's "no PC required"
  design intact.
- Interface is selected via two switches/jumpers on the board (`SEL0`,
  `SEL1`):

  | Interface | SEL0 | SEL1 |
  |---|---|---|
  | UART | 0 | 0 |
  | SPI  | 0 | 1 |
  | I2C  | 1 | 0 |

  SPI is recommended for this project: it's the most reliable/best
  supported option, and unlike UART it doesn't compete with the Uno's
  single hardware serial port (which the sketch currently uses for
  debug logging via `Serial`).
- `InListPassiveTarget` performs Wake up + Anti-collision + Select in
  one call. RATS/ATS can then be obtained either automatically (PN532
  has an automatic-RATS mode) or explicitly via `InDataExchange` with a
  raw RATS APDU (`0xE0` + parameter byte).
- Known implementation detail to watch for: some PN532 Arduino
  libraries default to a 64-byte TX/RX buffer (`PN532_PACKBUFFSIZ`),
  and some larger EMV/DESFire exchanges need it increased along with a
  longer timeout. ATS itself is short, well under 64 bytes, so this
  isn't a blocker for ATS specifically — only relevant if the project
  later exchanges larger APDUs after ATS.
- Actively maintained libraries exist: `Adafruit_PN532`,
  `elechouse/PN532` (SPI/I2C/HSU variants).

### Option B: ACR128U (does not fit this project's architecture)

- ACRxxx-series dual-interface reader with native ISO14443-4/T=CL
  support and a built-in SAM slot.
- Can retrieve ATS, but only through **PC/SC**, which requires a PC
  host with PC/SC drivers (Windows/Linux/macOS) — not something an
  Arduino Uno can talk to directly. Connects to a PC via USB, not to a
  microcontroller.
- Would require restructuring this project into a PC-based application
  (C#, Python `pyscard`, Java `javax.smartcardio`, etc.), abandoning the
  "standalone Arduino, no PC required" design goal stated in this
  project's README. Not recommended unless that architecture change is
  explicitly desired for other reasons.

### Ethernet-connected ATS-capable readers

No verified, documented product was found in this category. ATS/RATS
support is common in embeddable RFID front-end chips meant to be wired
directly to a microcontroller (PN532, MFRC531, TI TRF7960/TRF7963A,
ST25R3911), and separately common in PC/SC USB readers (ACR128U and
similar) — but not in standalone Ethernet-connected reader modules like
the RRHFOEM04. Ethernet RFID readers on the market are built for
access-control use cases (UID-only) almost exclusively. If Ethernet
connectivity is a hard requirement, the practical path is to keep PN532
on SPI for the card-reading side and add a separate Ethernet
shield/module (e.g. W5500-based, same family as this project's current
shield) purely to report the pass/fail result over the network,
reusing this project's existing networking pattern rather than
depending on a single device to do both jobs.

## Which card types return UID, ATS, both, or neither

The deciding factor for ATS support is the **SAK** (Select
Acknowledge) byte returned during Anti-collision/Select: bit 6 (mask
`0x20`) set means the card supports ISO14443-4 and will respond to
RATS; bit 6 clear means it will not, confirmed via a maintainer note on
the [Flipper Zero firmware repo](https://github.com/flipperdevices/flipperzero-firmware/issues/1472).
| Card type | Typical use case | Returns UID? | Returns ATS? | Notes |
|---|---|---|---|---|
| MIFARE Classic (1K/4K/Mini) | Transit cards, access badges, loyalty cards | Yes | No | ISO14443-3 only, SAK bit 6 not set. Documented in this repo's own `RRHFOEM04-TCP/Mifare 1K Card IC Datasheet`. |
| MIFARE Ultralight / Ultralight C | Disposable/limited-use transit tickets, event passes | Yes | No | ISO14443-3 only, no RATS support. Very common for single-ride/cheap transit cards. |
| NTAG213 / NTAG215 / NTAG216 | Transit cards, NFC tags, smart posters | Yes | No | Same family behavior as Ultralight; ISO14443-3 only. |
| MIFARE Plus (Security Level 1) | Upgraded transit/access cards in Classic-compatible mode | Yes | No | SL1 mode behaves like Classic — ISO14443-3 only. |
| MIFARE Plus (Security Level 3) | Upgraded transit/access cards, full security mode | Yes | Yes | SL3 switches to ISO14443-4, SAK bit 6 set. |
| MIFARE DESFire (EV1/EV2/EV3), DESFire Light | Multi-application transit cards (stored-value wallets), ID cards | Yes | Yes | Built on ISO14443-4 to support APDU-based file access. A 7-byte UID alone does not distinguish DESFire from Ultralight/NTAG — only the SAK does. This is the family that can make a "metro card" behave like an EMV card for ATS purposes. |
| EMV contactless bank cards (Visa payWave, Mastercard PayPass, RuPay, etc.) | Debit/credit cards | Yes | Yes | Requires ISO14443-4 for EMV APDU exchange (SELECT, GPO, READ RECORD, etc.). This is the card type behind the original UID-read investigation in `docs/troubleshooting-uid-read.md`. |
| ISO15693 vicinity cards (e.g. ICODE, TAG-IT) | Access control, asset tracking, library systems | Yes (via ISO15693 Inventory, not ISO14443) | No | Different RF standard/framing from ISO14443 entirely. The RRHFOEM04 test app has a separate "ISO15693" tab for this. |
| ISO14443 Type B cards | Some government ID cards, e-passports, a minority of transit systems | Yes (via Type B anti-collision) | Sometimes | Uses `ATTRIB` instead of `RATS`/`WUPA`/`REQA`; response is functionally similar to ATS but via a different command path. This project's reader/sketch only implement Type A today, so Type B cards aren't detected at all currently. |
| Damaged or miscoupled card (any chip type) | Bent/cracked antenna, RFID-blocking sleeve, or heavy shielding from other cards stacked in a wallet | No | No | Physical-layer failure mode observed with the specific personal debit card during testing (see `docs/troubleshooting-uid-read.md`). The card never completes Wake up/Anti-collision, so no reader — regardless of protocol support — can get a UID or ATS from it. |

### Practical implication for this project

- If the goal is detecting **any 13.56 MHz ISO14443-3/4 card is alive**
  (transit + bank cards both), **UID must remain the primary signal**,
  since Classic/Ultralight/NTAG cards — common for transit/metro use —
  will never return an ATS no matter what reader is used.
- ATS is a **useful additional signal only for the subset of cards that
  support ISO14443-4** (DESFire, MIFARE Plus SL3, EMV bank cards). It
  cannot replace UID-based detection as a general "is this card alive"
  check without excluding a large class of legitimately working cards.
- A card returning **neither** UID nor ATS is the case actually seen in
  testing with one specific personal card — that points to a physical
  RF coupling problem with that card/environment, not a protocol gap,
  and is not fixed by switching reader hardware or detection method.

## Impact on "alive or not" detection logic

The project's existing pass/fail design doesn't need to change
structurally — only what counts as success:

- **Today:** success = a UID was returned by Inventory/Anti-collision.
- **Proposed:** success = ATS was returned in response to RATS (i.e.
  the card completed ISO14443-3 activation **and** confirmed
  ISO14443-4 support).

The trigger button, debounce logic, non-blocking `millis()` timing, and
the two-output-pin signaling (`outputUidFoundPin` / `outputNoUidPin`)
can all be reused as-is conceptually — only the reader communication
layer (currently hand-rolled TCP framing/CRC for the RRHFOEM04) would
be replaced with PN532 library calls over SPI.

## Trade-offs to weigh before committing to this direction

1. **Not all cards will produce an ATS.** Basic MIFARE
   Classic/Ultralight cards (common for transit/metro cards) generally
   do not support ISO14443-4, so they would not return an ATS even
   though they are legitimately "alive." If the project needs to
   detect aliveness across *all* card types (not just EMV/bank cards),
   ATS-only detection would need a fallback to UID-based detection for
   non-ISO14443-4 cards, rather than fully replacing it.
2. **Hardware change required.** This is not a firmware-only fix; it
   requires physically swapping the RRHFOEM04 for a PN532 module (or
   adding one alongside it) and rewriting the reader communication
   layer of the sketch.
3. **The known-bad personal debit card issue is independent of this
   change.** If a specific card fails to even complete Wake
   up/Anti-collision (as observed in testing), no reader — RRHFOEM04,
   PN532, or otherwise — will get an ATS from it either, since ATS
   requires a successful activation first. That card-specific issue
   needs to be resolved on its own (checking for wallet interference,
   physical antenna damage, positioning) regardless of which reader is
   used.

## Recommendation

If the goal is specifically to distinguish "this is a proper
EMV/ISO14443-4 card and it's alive" (e.g. for bank-card-focused
detection), moving to a PN532-based reader and checking for ATS is a
reasonable, well-supported direction, and keeps the project's
Arduino-only, no-PC architecture intact.

If the goal is general "is any 13.56 MHz card present and responsive"
(including transit/metro-type cards), UID-based detection should remain
the primary signal, with ATS treated as an additional data point for
cards that support it — not a full replacement.















| Card Type                                                                      | Typical Use Case                                                                          | Returns UID? | Returns ATS? | Notes                                                                                                                                       |
| ------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------- | ------------ | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------- |
| **MIFARE Classic (1K / 4K / Mini)**                                            | Transit cards, access badges, loyalty cards                                               | Yes          | No           | ISO14443-3 only; SAK bit 6 not set. Documented in the project's *Mifare 1K Card IC Datasheet*.                                              |
| **MIFARE Ultralight / Ultralight C**                                           | Disposable or limited-use transit tickets, event passes                                   | Yes          | No           | ISO14443-3 only; does not support RATS. Common for low-cost transit tickets.                                                                |
| **NTAG213 / NTAG215 / NTAG216**                                                | Transit cards, NFC tags, smart posters                                                    | Yes          | No           | Same family behavior as Ultralight; ISO14443-3 only.                                                                                        |
| **MIFARE Plus (Security Level 1)**                                             | Upgraded transit and access cards operating in Classic-compatible mode                    | Yes          | No           | SL1 mode behaves like MIFARE Classic; ISO14443-3 only.                                                                                      |
| **MIFARE Plus (Security Level 3)**                                             | Upgraded transit and access cards operating in full-security mode                         | Yes          | Yes          | SL3 mode switches to ISO14443-4; SAK bit 6 is set.                                                                                          |
| **MIFARE DESFire (EV1 / EV2 / EV3) & DESFire Light**                           | Multi-application transit cards, stored-value wallets, ID cards                           | Yes          | Yes          | Built on ISO14443-4 to support APDU-based file access. A 7-byte UID alone cannot distinguish DESFire from Ultralight/NTAG; SAK is required. |
| **EMV Contactless Bank Cards (Visa payWave, Mastercard PayPass, RuPay, etc.)** | Debit cards, credit cards, payment cards                                                  | Yes          | Yes          | Uses ISO14443-4 for EMV APDU exchange (SELECT, GPO, READ RECORD, etc.).                                                                     |
| **ISO15693 Vicinity Cards (ICODE, TAG-IT, etc.)**                              | Access control, asset tracking, library systems                                           | Yes          | No           | Uses ISO15693 Inventory rather than ISO14443 anti-collision. Different RF protocol and framing.                                             |
| **ISO14443 Type B Cards**                                                      | Government IDs, e-passports, some transit systems                                         | Yes          | Sometimes    | Uses `ATTRIB` instead of `RATS`, `REQA`, or `WUPA`. Similar functionality to ATS but through a different protocol path.                     |
| **Damaged or Miscoupled Cards (Any Chip Type)**                                | Cards with damaged antennas, RFID-blocking sleeves, or heavy shielding from stacked cards | No           | No           | Physical-layer failure. The card never completes wake-up or anti-collision, preventing both UID and ATS retrieval.                          |
