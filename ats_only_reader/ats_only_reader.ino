#include <SPI.h>
#include <Adafruit_PN532.h>

// ================= PN532 Wiring (Hardware SPI) =================
// Same pin configuration as dead_card_detector.ino.
// Uno hardware SPI pins are fixed: SCK=13, MOSI=11, MISO=12.
// Only the SS (chip select) pin is configurable.
const uint8_t PN532_SS = 10;  // PN532 chip select
Adafruit_PN532 nfc(PN532_SS);

// ================= Pin Configuration =================
const uint8_t ledStatusPin   = 6;   // Reader-ready status LED
const uint8_t triggerPin     = 7;   // INPUT_PULLUP push button, LOW = pressed
const uint8_t outputAtsFoundPin = 8; // Latched HIGH when ATS is obtained, while trigger is held
const uint8_t outputNoAtsPin    = 9; // Latched HIGH when no card / no ATS, while trigger is held

// ================= Timing Configuration =================
const unsigned long debounceMs    = 50;
const uint8_t        passiveActivationRetries = 3; // Bounds PN532's internal poll
                                                     // retries so a trigger press with
                                                     // no card returns quickly instead
                                                     // of blocking.

// ================= RATS (Request for Answer To Select) =================
// ISO14443-4 command. Not every card supports this - see
// docs/ats-based-detection-proposal.md for which card types do/don't.
// Byte 0: RATS command (0xE0). Byte 1: parameter byte - upper nibble FSDI
// (frame size, 5 = 64 bytes), lower nibble CID (0 = no logical channel).
// Not declared const: Adafruit_PN532::inDataExchange() takes a non-const
// uint8_t* for the bytes to send.
uint8_t CMD_RATS[2] = { 0xE0, 0x50 };

int  triggerRawState    = HIGH;
int  triggerStableState = HIGH;
unsigned long triggerLastChangeMs = 0;

// True while the trigger is held down AND a read has been performed for
// this hold. Used to latch the result output pins for the whole hold
// duration, and to make sure a held trigger doesn't re-trigger reads
// on every loop() iteration.
bool triggerHoldReadDone = false;

void setup() {
  pinMode(ledStatusPin, OUTPUT);
  digitalWrite(ledStatusPin, LOW);

  pinMode(triggerPin, INPUT_PULLUP);

  pinMode(outputAtsFoundPin, OUTPUT);
  digitalWrite(outputAtsFoundPin, LOW);
  pinMode(outputNoAtsPin, OUTPUT);
  digitalWrite(outputNoAtsPin, LOW);

  Serial.begin(9600);

  Serial.println(F("Initializing PN532..."));
  nfc.begin();

  uint32_t versionData = nfc.getFirmwareVersion();
  if (!versionData) {
    Serial.println(F("Could not find PN532 board. Check wiring."));
    while (1) {
      // Halt: nothing else can proceed without the reader.
    }
  }

  Serial.print(F("Found PN5"));
  Serial.println((versionData >> 24) & 0xFF, HEX);
  Serial.print(F("Firmware version: "));
  Serial.print((versionData >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versionData >> 8) & 0xFF, DEC);

  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(passiveActivationRetries);

  digitalWrite(ledStatusPin, HIGH);
  Serial.println(F("Reader ready. This sketch checks ATS only (no UID output)."));
}

void loop() {
  updateTriggerState();

  if (triggerStableState == LOW && !triggerHoldReadDone) {
    // Trigger has just been (debounced) pressed and no read has been
    // done yet for this hold - do the read now. The result stays
    // latched on the output pins for as long as the button stays held.
    triggerHoldReadDone = true;
    processAtsRead();
  } else if (triggerStableState == HIGH && triggerHoldReadDone) {
    // Trigger released - clear the latched result and go back to
    // ready state.
    triggerHoldReadDone = false;
    resetOutputs();
  }
}

// ================= Trigger-Driven ATS Read =================

// Debounces the trigger pin and updates triggerStableState in place.
void updateTriggerState() {
  int current = digitalRead(triggerPin);
  unsigned long now = millis();

  if (current != triggerRawState) {
    triggerRawState = current;
    triggerLastChangeMs = now;
  }

  if (now - triggerLastChangeMs > debounceMs && triggerStableState != triggerRawState) {
    triggerStableState = triggerRawState;
  }
}

void processAtsRead() {
  unsigned long readStartMs = millis();
  Serial.println(F("Trigger pressed. Checking for ATS..."));

  // Force a fresh PN532 wakeup/SAM re-config before each attempt.
  //
  // Why this is needed: per ISO14443-4, RATS is only valid once per card
  // activation session. Once a card has answered RATS, it is in the
  // "active" ISO-DEP state, and sending RATS again without first
  // deactivating (InRelease/InDeselect) and re-activating the card from
  // scratch is out of spec - most cards reject or ignore a second RATS
  // on the same activation. This is why, without this re-init, ATS only
  // succeeds on the first trigger press for a card left sitting on the
  // reader, and fails on every subsequent press until the card is
  // physically lifted and re-presented.
  //
  // The Adafruit_PN532 library does not expose a public InRelease/
  // InDeselect method (the PN532 chip supports it, but this library
  // doesn't wrap it, and the low-level read/write functions needed to
  // build it manually are private). Re-running begin()+SAMConfig() is
  // the closest available workaround using only the public API - it
  // re-runs the PN532's wakeup sequence, which should reset its RF
  // activation state enough to make RATS answerable again without
  // requiring the card to be removed. This has not been verified
  // against real hardware; if a card still fails to return ATS a
  // second time after this, physically lifting and re-presenting the
  // card between reads remains the reliable fallback.
  nfc.begin();
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(passiveActivationRetries);

  // inListPassiveTarget() (no UID argument) is used here instead of
  // readPassiveTargetID(). This is deliberate: inDataExchange() (needed
  // for RATS below) depends on the library's internal _inListedTag
  // value, which only inListPassiveTarget() sets correctly.
  // readPassiveTargetID() does NOT set it, so calling that first and then
  // trying inDataExchange() afterward fails reliably - this was confirmed
  // while testing dead_card_detector.ino's UID+ATS combined attempt.
  // Using inListPassiveTarget() alone, as the only activation step,
  // avoids that problem entirely.
  bool cardPresent = nfc.inListPassiveTarget();

  if (!cardPresent) {
    Serial.println(F("No card detected."));
    printElapsedMs(readStartMs);
    digitalWrite(outputNoAtsPin, HIGH);
    return;
  }

  Serial.println(F("Card activated. Sending RATS..."));

  uint8_t atsResponse[32];
  uint8_t atsResponseLength = sizeof(atsResponse);

  bool gotAts = nfc.inDataExchange(CMD_RATS, sizeof(CMD_RATS), atsResponse, &atsResponseLength);

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

// ================= Output Reset =================

// Clears both result output pins, returning the reader to its ready
// (no output) state. Called the moment the trigger is released.
void resetOutputs() {
  digitalWrite(outputAtsFoundPin, LOW);
  digitalWrite(outputNoAtsPin, LOW);
}

// ================= Logging Helper =================

void printElapsedMs(unsigned long startMs) {
  Serial.print(F("Time to result: "));
  Serial.print(millis() - startMs);
  Serial.println(F(" ms"));
}

void printHexBytes(const uint8_t* data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}
