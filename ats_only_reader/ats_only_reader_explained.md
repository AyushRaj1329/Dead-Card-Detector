# `ats_only_reader.ino` — Line by Line Explanation

This document walks through every part of `ats_only_reader.ino`: an Arduino sketch that uses a PN532 NFC/RFID reader to check whether a card responds to RATS (Request for Answer To Select), an ISO14443-4 command. The reader is triggered by a push button: hold the button to read and hold the result on the output pins, release the button to reset.

---

## Includes and object setup

```cpp
#include <SPI.h>
#include <Adafruit_PN532.h>
```
- `SPI.h`: Arduino's built-in library for the SPI bus, which is how the Uno talks to the PN532 chip.
- `Adafruit_PN532.h`: Adafruit's driver library for the PN532 chip. Provides the `Adafruit_PN532` class used below.

```cpp
const uint8_t PN532_SS = 10;
Adafruit_PN532 nfc(PN532_SS);
```
- `PN532_SS`: the SPI chip-select (slave select) pin, wired to pin 10. This is the only SPI pin that's configurable on an Uno — `SCK` (13), `MOSI` (11), and `MISO` (12) are fixed by the hardware SPI peripheral.
- `nfc`: the global PN532 driver object, constructed for hardware SPI using pin 10 as chip select. Every interaction with the reader goes through this object.

---

## Pin configuration

```cpp
const uint8_t ledStatusPin   = 6;
const uint8_t triggerPin     = 7;
const uint8_t outputAtsFoundPin = 8;
const uint8_t outputNoAtsPin    = 9;
```
- `ledStatusPin` (6): drives an LED that turns on once the reader has finished initializing, as a simple "ready" indicator.
- `triggerPin` (7): a push button wired with `INPUT_PULLUP`, meaning it reads `HIGH` when not pressed and `LOW` when pressed (button pulls the pin to ground).
- `outputAtsFoundPin` (8): goes `HIGH` and stays `HIGH` for as long as the trigger is held, if the card responded to RATS (i.e. it supports ISO14443-4).
- `outputNoAtsPin` (9): goes `HIGH` and stays `HIGH` for as long as the trigger is held, if no card was found, or the card didn't answer RATS.

---

## Timing configuration

```cpp
const unsigned long debounceMs    = 50;
const uint8_t        passiveActivationRetries = 3;
```
- `debounceMs`: how long (in milliseconds) the trigger pin's signal must stay stable before a press/release is accepted as real. This filters out electrical noise/bounce from the mechanical button.
- `passiveActivationRetries`: passed to the PN532's own internal retry counter for card polling. Capped at 3 instead of the library default (which can retry effectively forever) so that pressing the trigger with no card present returns quickly instead of hanging.

---

## RATS command bytes

```cpp
uint8_t CMD_RATS[2] = { 0xE0, 0x50 };
```
- This is the raw 2-byte RATS command sent to an already-activated card:
  - `0xE0`: the RATS command code itself (fixed by the ISO14443-4 spec).
  - `0x50`: the parameter byte. Upper nibble (`5`) is FSDI, which tells the card the reader can accept frames up to 64 bytes. Lower nibble (`0`) is CID (logical channel ID), set to 0 meaning no logical channel is used.
- It's declared as a mutable array (not `const`) because `Adafruit_PN532::inDataExchange()` expects a non-const `uint8_t*` argument — the library's function signature doesn't accept a `const` pointer even though this sketch never modifies the bytes.

---

## Global state variables

```cpp
int  triggerRawState    = HIGH;
int  triggerStableState = HIGH;
unsigned long triggerLastChangeMs = 0;
```
- `triggerRawState`: the last raw value read directly off the trigger pin, before debouncing.
- `triggerStableState`: the debounced, "trusted" state of the trigger — this is what the rest of the code acts on.
- `triggerLastChangeMs`: timestamp (from `millis()`) of the last time the raw pin value changed, used to measure how long the signal has been stable.

```cpp
bool triggerHoldReadDone = false;
```
- Tracks whether a read has already been performed for the *current* button hold. Prevents the sketch from re-running the (relatively slow) ATS read over and over every loop iteration while the button stays pressed. Reset to `false` the moment the button is released.

---

## `setup()`

```cpp
void setup() {
  pinMode(ledStatusPin, OUTPUT);
  digitalWrite(ledStatusPin, LOW);
```
Configures the status LED pin as an output and makes sure it starts off (reader isn't ready yet).

```cpp
  pinMode(triggerPin, INPUT_PULLUP);
```
Configures the trigger pin as an input with the internal pull-up resistor enabled, so it idles `HIGH` and reads `LOW` when the button is pressed (button connects the pin to ground).

```cpp
  pinMode(outputAtsFoundPin, OUTPUT);
  digitalWrite(outputAtsFoundPin, LOW);
  pinMode(outputNoAtsPin, OUTPUT);
  digitalWrite(outputNoAtsPin, LOW);
```
Configures both result pins as outputs and forces them low at startup, so nothing is asserted before any read has happened.

```cpp
  Serial.begin(9600);
```
Starts the serial connection at 9600 baud, used purely for debug/status logging to the Serial Monitor.

```cpp
  Serial.println(F("Initializing PN532..."));
  nfc.begin();
```
Logs a startup message, then initializes the PN532 driver: sets up the SPI bus, performs the chip's wakeup sequence, and gets it into a state where it can be talked to. (`F()` stores the string in flash instead of RAM, since RAM is scarce on an Uno.)

```cpp
  uint32_t versionData = nfc.getFirmwareVersion();
  if (!versionData) {
    Serial.println(F("Could not find PN532 board. Check wiring."));
    while (1) {
      // Halt: nothing else can proceed without the reader.
    }
  }
```
Asks the chip for its firmware version as a sanity check that it's actually connected and responding. If the response is `0` (failure), the sketch prints an error and halts forever in an empty `while(1)` loop — there's no point continuing without a working reader.

```cpp
  Serial.print(F("Found PN5"));
  Serial.println((versionData >> 24) & 0xFF, HEX);
  Serial.print(F("Firmware version: "));
  Serial.print((versionData >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versionData >> 8) & 0xFF, DEC);
```
`versionData` packs several fields into one 32-bit value. This unpacks and prints them:
- Bits 31-24: the chip variant (e.g. `0x32` → "PN532").
- Bits 23-16: firmware major version.
- Bits 15-8: firmware minor version.
- (Bits 7-0, unused here, hold additional support-flag data.)

```cpp
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(passiveActivationRetries);
```
- `SAMConfig()`: configures the PN532's Secure Access Module into normal reader mode — required once after wakeup before the chip will do passive card polling correctly.
- `setPassiveActivationRetries()`: applies the retry cap defined earlier (3), so future polling won't block for a long time when no card is present.

```cpp
  digitalWrite(ledStatusPin, HIGH);
  Serial.println(F("Reader ready. This sketch checks ATS only (no UID output)."));
}
```
Turns the status LED on to signal the reader has finished initializing, and logs that it's ready.

---

## `loop()`

```cpp
void loop() {
  updateTriggerState();
```
Every loop iteration, first refresh the debounced trigger state by reading the pin and running it through the debounce logic (see `updateTriggerState()` below).

```cpp
  if (triggerStableState == LOW && !triggerHoldReadDone) {
    triggerHoldReadDone = true;
    processAtsRead();
  }
```
If the trigger is currently (debounced) pressed, and no read has been done yet for this hold, mark the read as "done for this hold" and perform the read. This runs exactly once per press — even if the button is held down for a long time, this branch won't re-fire.

```cpp
  else if (triggerStableState == HIGH && triggerHoldReadDone) {
    triggerHoldReadDone = false;
    resetOutputs();
  }
}
```
If the trigger is currently (debounced) released, and a read had been done for the previous hold, clear that flag and reset both output pins back to `LOW`. This is what makes the output pins drop the instant the button is let go.

---

## `updateTriggerState()`

```cpp
void updateTriggerState() {
  int current = digitalRead(triggerPin);
  unsigned long now = millis();
```
Reads the trigger pin's current raw value, and grabs the current time (milliseconds since the board booted).

```cpp
  if (current != triggerRawState) {
    triggerRawState = current;
    triggerLastChangeMs = now;
  }
```
If the raw reading has changed since last time, record the new raw value and reset the "last change" timestamp. This restarts the debounce timer any time the signal wiggles.

```cpp
  if (now - triggerLastChangeMs > debounceMs && triggerStableState != triggerRawState) {
    triggerStableState = triggerRawState;
  }
}
```
Only if the raw value has held steady for longer than `debounceMs` (50ms) — and it actually differs from the currently accepted stable state — does the debounced `triggerStableState` get updated. This is the standard debounce pattern: ignore brief signal noise, only trust a value once it's been stable for a while.

---

## `processAtsRead()`

```cpp
void processAtsRead() {
  unsigned long readStartMs = millis();
  Serial.println(F("Trigger pressed. Checking for ATS..."));
```
Records the start time (for the elapsed-time log at the end) and logs that a read is starting.

```cpp
  nfc.begin();
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(passiveActivationRetries);
```
Re-runs the PN532's wakeup and configuration sequence before every single read attempt. This is a deliberate workaround (explained in the long comment block in the source file): per ISO14443-4, RATS is only valid once per card "activation session." A card that's already answered RATS won't answer it again unless it's deactivated and reactivated first. The Adafruit library doesn't expose a public deactivate function, so re-running `begin()` + `SAMConfig()` is used instead — it resets the chip's RF activation state closely enough to make RATS answerable again on a card that's still sitting on the reader, without requiring the card to be physically lifted and re-presented.

```cpp
  bool cardPresent = nfc.inListPassiveTarget();
```
Polls for a passive (unpowered) ISO14443A card in the field and, if found, fully activates it. This function is used specifically because it also sets the PN532 library's internal "currently listed tag" state, which `inDataExchange()` (used just below for RATS) depends on. The alternative function, `readPassiveTargetID()`, does *not* set that internal state, so calling it first would make the subsequent RATS exchange fail — this was confirmed the hard way while building an earlier combined UID + ATS sketch (`dead_card_detector.ino`, since removed from the repo; see [`../docs/ats-based-detection-proposal.md`](../docs/ats-based-detection-proposal.md) for that finding).

```cpp
  if (!cardPresent) {
    Serial.println(F("No card detected."));
    printElapsedMs(readStartMs);
    digitalWrite(outputNoAtsPin, HIGH);
    return;
  }
```
If no card was found: log it, print how long the whole read took, latch `outputNoAtsPin` high, and exit the function early (skip the RATS step entirely — there's no card to send it to).

```cpp
  Serial.println(F("Card activated. Sending RATS..."));

  uint8_t atsResponse[32];
  uint8_t atsResponseLength = sizeof(atsResponse);
```
A card was found and activated. Log that, then set up a 32-byte buffer to receive the card's ATS response, along with a variable telling the library how big that buffer is (the library will overwrite this with the actual response length).

```cpp
  bool gotAts = nfc.inDataExchange(CMD_RATS, sizeof(CMD_RATS), atsResponse, &atsResponseLength);
```
Sends the 2-byte RATS command (`CMD_RATS`) to the activated card and waits for a response, storing it in `atsResponse` and updating `atsResponseLength` to the actual number of bytes received. Returns `true` if the exchange succeeded, `false` if it failed or timed out (e.g. the card doesn't support ISO14443-4).

```cpp
  if (gotAts) {
    Serial.print(F("ATS obtained: "));
    printHexBytes(atsResponse, atsResponseLength);
    printElapsedMs(readStartMs);
    digitalWrite(outputAtsFoundPin, HIGH);
  } else {
    Serial.println(F("No ATS (card does not support ISO14443-4, or RATS failed)."));
    printElapsedMs(readStartMs);
    digitalWrite(outputNoAtsPin, HIGH);
  }
}
```
- If RATS succeeded: log the raw ATS bytes as hex, print elapsed time, and latch `outputAtsFoundPin` high.
- If it failed: log that no ATS was obtained, print elapsed time, and latch `outputNoAtsPin` high instead.

Either way, whichever pin gets set stays `HIGH` until the trigger is released and `resetOutputs()` runs (back in `loop()`).

---

## `resetOutputs()`

```cpp
void resetOutputs() {
  digitalWrite(outputAtsFoundPin, LOW);
  digitalWrite(outputNoAtsPin, LOW);
}
```
Drives both result pins low. Called the instant the trigger's debounced release is detected, returning the sketch to its "ready, no output" state.

---

## `printElapsedMs()`

```cpp
void printElapsedMs(unsigned long startMs) {
  Serial.print(F("Time to result: "));
  Serial.print(millis() - startMs);
  Serial.println(F(" ms"));
}
```
Debug helper: prints how many milliseconds elapsed between `startMs` (captured at the top of `processAtsRead()`) and now. Used to measure how long a full read (re-init + card poll + optional RATS exchange) actually takes.

---

## `printHexBytes()`

```cpp
void printHexBytes(const uint8_t* data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}
```
Debug helper: prints an arbitrary byte array as space-separated hex pairs (e.g. `06 75 77 81 02 80`). The `if (data[i] < 0x10) Serial.print('0')` line pads single-hex-digit values with a leading zero, so `0x06` prints as `06` instead of just `6`, keeping the output visually aligned.

---

## Overall behavior summary

1. On boot: initialize the PN532, verify it responds, turn on the ready LED.
2. Every `loop()` iteration: debounce the trigger button.
3. On press (debounced): re-init the PN532, poll for a card, and if found, send RATS. Latch the matching output pin (`outputAtsFoundPin` or `outputNoAtsPin`) high based on the result. This runs once per press, not repeatedly while held.
4. While held: outputs stay latched at whatever result was determined; no repeated reads happen.
5. On release (debounced): both output pins drop back to `LOW`, and the sketch is ready for the next press.
