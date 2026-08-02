# ATS-based detection — design decision and card support reference

> **Status: implemented.** This started as a proposal to move from UID-based
> to ATS-based detection. That decision was taken and shipped — the current
> firmware is [`ats_only_reader/ats_only_reader.ino`](../ats_only_reader/ats_only_reader.ino),
> which uses a PN532 over SPI and signals on ATS. The filename is kept as-is
> because the sketch source and other docs link to it.
>
> The card-type support table below is the part of this document that stays
> operationally useful. The rest records why the design is the way it is.

## Background: why UID-based detection was replaced

The original firmware determined whether a card was "alive" by reading its
**UID** through an Ethernet-connected RRHFOEM04 reader. Two separate
problems drove the change:

1. **Inconsistent UID reads across cards.** Some cards failed to return a
   UID even when physically present. This was traced to physical/RF factors
   (card positioning, wallet interference from adjacent cards, possible
   antenna wear) rather than a firmware bug — see
   [`troubleshooting-uid-read.md`](troubleshooting-uid-read.md) for the full
   investigation.
2. **The RRHFOEM04 had a hard protocol ceiling.** Its command set only
   implemented **ISO14443-3** (Request, Wake up, Anti-collision, Select
   Card, Inventory, Halt). It had no RATS/ATS command at all, so ATS was
   unreachable on that hardware regardless of firmware changes.

## What ATS actually is

- **UID** (Unique Identifier) is returned during ISO14443-3
  Anti-collision. It's just the card's serial number — no cryptography, no
  application-level exchange. Getting a UID confirms the card powered up
  and responded to basic RF addressing, nothing more.
- **ATS** (Answer To Select) is returned in response to a **RATS**
  (Request for Answer To Select) command, which is the entry point to
  **ISO14443-4** (the "T=CL" transport protocol layer). ATS tells the
  reader the card's supported frame size, timing parameters, and protocol
  capabilities so that higher-level APDU exchange (the kind EMV payment
  cards and DESFire cards use) can begin.
- Practically: **UID confirms an ISO14443-3 card is present. ATS confirms
  the card additionally supports ISO14443-4** — a stronger, deeper
  handshake than UID alone. Not every 13.56 MHz card supports ISO14443-4
  (e.g. plain MIFARE Classic/Ultralight cards, common on transit/metro
  cards, typically do not), so ATS is not a strict superset that works for
  every card type; it's a different signal that happens to be more
  informative for cards that do support it.

## Why the previous reader (RRHFOEM04) could not do this

Confirmed at the time by inspecting the manufacturer's own reference
application source (`frmMain.cs` from RapidRadio's RRHFOEM04 CS test app,
which was vendored into the repo during the investigation and has since
been removed) — there was no RATS/ATS command anywhere in its command set
or protocol. This was a limitation of the reader itself, not of the
firmware driving it. Getting ATS required different reader hardware.

## Hardware decision

### Chosen: PN532

- NXP PN532 NFC/RFID front-end chip, widely available as Arduino-ready
  breakout modules (Adafruit, Elechouse, generic clones).
- **Connects directly to Arduino Uno R3** via SPI, I2C, or UART — no
  separate PC/host needed, preserving the project's "no PC required"
  design.
- Interface is selected via two switches/jumpers on the board (`SEL0`,
  `SEL1`):

  | Interface | SEL0 | SEL1 |
  |---|---|---|
  | UART | 0 | 0 |
  | SPI  | 0 | 1 |
  | I2C  | 1 | 0 |

  SPI was chosen: it's the most reliable/best supported option, and unlike
  UART it doesn't compete with the Uno's single hardware serial port, which
  the sketch uses for debug logging.
- `InListPassiveTarget` performs Wake up + Anti-collision + Select in one
  call; RATS is then sent via `InDataExchange`.
- Actively maintained libraries exist: `Adafruit_PN532` (used here),
  `elechouse/PN532`.

See [`../ats_only_reader/wiring-connections.md`](../ats_only_reader/wiring-connections.md)
for the resulting wiring.

### Considered and rejected: ACR128U

- ACRxxx-series dual-interface reader with native ISO14443-4/T=CL support
  and a built-in SAM slot. Can retrieve ATS.
- Rejected because it only exposes this through **PC/SC**, which requires a
  PC host with PC/SC drivers. It connects to a PC via USB, not to a
  microcontroller, so it cannot be driven by an Arduino Uno directly.
  Adopting it would have meant restructuring the project into a PC-based
  application (C#, Python `pyscard`, Java `javax.smartcardio`), abandoning
  the standalone-Arduino design goal.

### Considered: Ethernet-connected ATS-capable readers

No verified, documented product was found in this category. ATS/RATS
support is common in embeddable RFID front-end chips meant to be wired
directly to a microcontroller (PN532, MFRC531, TI TRF7960/TRF7963A,
ST25R3911), and separately common in PC/SC USB readers (ACR128U and
similar) — but not in standalone Ethernet-connected reader modules like the
RRHFOEM04. Ethernet RFID readers on the market are built for access-control
use cases (UID-only) almost exclusively.

If Ethernet connectivity is needed again later, the practical path is to
keep the PN532 on SPI for card reading and add a separate Ethernet
shield/module (e.g. W5500-based) purely to report the pass/fail result over
the network — rather than depending on one device to do both jobs.

## Which card types return UID, ATS, both, or neither

The deciding factor for ATS support is the **SAK** (Select Acknowledge)
byte returned during Anti-collision/Select: bit 6 (mask `0x20`) set means
the card supports ISO14443-4 and will respond to RATS; bit 6 clear means it
will not, confirmed via a maintainer note on the
[Flipper Zero firmware repo](https://github.com/flipperdevices/flipperzero-firmware/issues/1472).

| Card Type                                                                      | Typical Use Case                                                                          | Returns UID? | Returns ATS? | Notes                                                                                                                                       |
| ------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------- | ------------ | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------- |
| **MIFARE Classic (1K / 4K / Mini)**                                            | Transit cards, access badges, loyalty cards                                               | Yes          | No           | ISO14443-3 only; SAK bit 6 not set.                                                                                                         |
| **MIFARE Ultralight / Ultralight C**                                           | Disposable or limited-use transit tickets, event passes                                   | Yes          | No           | ISO14443-3 only; does not support RATS. Common for low-cost transit tickets.                                                                |
| **NTAG213 / NTAG215 / NTAG216**                                                | Transit cards, NFC tags, smart posters                                                    | Yes          | No           | Same family behavior as Ultralight; ISO14443-3 only.                                                                                        |
| **MIFARE Plus (Security Level 1)**                                             | Upgraded transit and access cards operating in Classic-compatible mode                    | Yes          | No           | SL1 mode behaves like MIFARE Classic; ISO14443-3 only.                                                                                      |
| **MIFARE Plus (Security Level 3)**                                             | Upgraded transit and access cards operating in full-security mode                         | Yes          | Yes          | SL3 mode switches to ISO14443-4; SAK bit 6 is set.                                                                                          |
| **MIFARE DESFire (EV1 / EV2 / EV3) & DESFire Light**                           | Multi-application transit cards, stored-value wallets, ID cards                           | Yes          | Yes          | Built on ISO14443-4 to support APDU-based file access. A 7-byte UID alone cannot distinguish DESFire from Ultralight/NTAG; SAK is required. |
| **EMV Contactless Bank Cards (Visa payWave, Mastercard PayPass, RuPay, etc.)** | Debit cards, credit cards, payment cards                                                  | Yes          | Yes          | Uses ISO14443-4 for EMV APDU exchange (SELECT, GPO, READ RECORD, etc.).                                                                     |
| **ISO15693 Vicinity Cards (ICODE, TAG-IT, etc.)**                              | Access control, asset tracking, library systems                                           | Yes          | No           | Uses ISO15693 Inventory rather than ISO14443 anti-collision. Different RF protocol and framing.                                             |
| **ISO14443 Type B Cards**                                                      | Government IDs, e-passports, some transit systems                                         | Yes          | Sometimes    | Uses `ATTRIB` instead of `RATS`, `REQA`, or `WUPA`. Similar functionality to ATS but through a different protocol path.                     |
| **Damaged or Miscoupled Cards (Any Chip Type)**                                | Cards with damaged antennas, RFID-blocking sleeves, or heavy shielding from stacked cards | No           | No           | Physical-layer failure. The card never completes wake-up or anti-collision, preventing both UID and ATS retrieval.                          |

## Consequences of choosing ATS as the signal

These are the accepted trade-offs of the shipped design, not open questions:

1. **Not every live card returns an ATS.** MIFARE Classic, Ultralight, and
   NTAG chips — common in transit/metro cards and access badges — are
   ISO14443-3 only and read as "No ATS" despite being fully functional. The
   current firmware reports these on the "No ATS" output.

   If the requirement ever changes to "detect *any* contactless card
   regardless of type", UID must be the primary signal, with ATS as an
   additional data point only for cards that support it. That would mean
   reintroducing UID reading alongside ATS.
2. **A card returning neither UID nor ATS is a physical-layer problem.**
   Observed during testing with one specific bank card. No reader and no
   detection method fixes this — it needs the card presented individually,
   out of any wallet or RFID-blocking sleeve.

## What implementation confirmed

Findings from actually building this that weren't known when the decision
was made:

- **RATS is only valid once per card activation session.** Per ISO14443-4,
  a card that has already answered RATS will not answer again until it is
  deactivated and re-activated. Symptom: the first trigger press on a card
  succeeds, and subsequent presses fail until the card is physically lifted
  and re-presented.
- **The Adafruit_PN532 library exposes no public deactivate call.** The
  PN532 chip supports `InRelease`/`InDeselect`, but the library doesn't wrap
  them, and the low-level read/write helpers needed to issue them manually
  are private. The sketch re-runs `begin()` + `SAMConfig()` before each read
  as the closest available workaround. **This has not been verified against
  hardware across repeated reads** — if a second consecutive read on an
  unmoved card fails, lifting and re-presenting the card remains the
  reliable fallback.
- **`readPassiveTargetID()` and `inDataExchange()` don't compose.**
  `inDataExchange()` (needed for RATS) depends on the library's private
  `_inListedTag`, which only `inListPassiveTarget()` sets —
  `readPassiveTargetID()` does not. Attempting to read the UID first and
  then send RATS therefore fails reliably. This is why the current sketch
  uses `inListPassiveTarget()` alone as its single activation step, and does
  not report UIDs at all.
