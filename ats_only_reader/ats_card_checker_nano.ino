/*
 * ATS Card Checker - Arduino Nano
 * 
 * Single-button NFC card tester that checks if a card supports ATS (ISO14443-4).
 * Shows connection status at startup, then tests cards on button press.
 * 
 * Hardware Configuration:
 * - 1× PN532 NFC reader in SPI mode
 * - 1× Push button (trigger)
 * - 2× LEDs (green = good/ATS found, red = bad/no ATS)
 * - Arduino Nano
 * 
 * Pin Assignment:
 * ================
 * SPI Bus (Hardware SPI - Standard Digital Pins):
 *   D13 - SCK   (connects to PN532 SCK)
 *   D12 - MISO  (connects to PN532 MISO)
 *   D11 - MOSI  (connects to PN532 MOSI)
 *   D10 - SS    (connects to PN532 SS/SSEL - chip select)
 * 
 * Control & Indicators:
 *   D9  - Trigger button (INPUT_PULLUP, active LOW)
 *   A0  - Good LED (green - ATS found / connection successful)
 *   A1  - Bad LED (red - no ATS / no card / connection failed)
 * 
 * Power:
 *   5V  - PN532 VCC
 *   GND - PN532 GND
 * 
 * Operation:
 * ===========
 * 1. Power on → Green/Red LED shows connection status for 3 seconds
 * 2. After status display → System ready
 * 3. Place card on reader → Press trigger button
 * 4. Good LED (green) = Card has ATS (alive/valid)
 *    Bad LED (red) = No card or no ATS (dead/invalid)
 * 5. Hold button → LED stays lit
 * 6. Release button → LED turns off, ready for next test
 * 
 * Features:
 * - Startup connection test with visual feedback
 * - Trigger disabled during initialization
 * - Latched output (LED stays on while button held)
 * - Automatic re-initialization for repeated reads
 * - Adjustable status display duration
 */

#include <SPI.h>
#include <Adafruit_PN532.h>

// ================= PN532 Wiring (Hardware SPI - Standard Pins) =================
// Arduino Nano hardware SPI pins: SCK=13, MOSI=11, MISO=12
// SS (chip select) is configurable
const uint8_t PN532_SS = 10;  // D10 controls PN532 chip select
Adafruit_PN532 nfc(PN532_SS);

// ================= Pin Configuration =================
const uint8_t triggerPin = 9;   // D9 - Trigger button (INPUT_PULLUP, active LOW)
const uint8_t goodLedPin = A0;  // A0 - Good LED (green) - ATS found
const uint8_t badLedPin  = A1;  // A1 - Bad LED (red) - No ATS/no card

// ================= Timing Configuration =================
const unsigned long statusDisplayDurationMs = 3000; // Status display time (adjustable)
const unsigned long debounceMs = 50;
const uint8_t passiveActivationRetries = 3;

// ================= RATS Command =================
// ISO14443-4 Request for Answer To Select
uint8_t CMD_RATS[2] = { 0xE0, 0x50 };

// ================= State Variables =================
int  triggerRawState    = HIGH;
int  triggerStableState = HIGH;
unsigned long triggerLastChangeMs = 0;
bool triggerHoldReadDone = false;
bool systemReady = false;

// ================= Setup =================
void setup() {
  pinMode(triggerPin, INPUT_PULLUP);

  pinMode(goodLedPin, OUTPUT);
  digitalWrite(goodLedPin, LOW);
  pinMode(badLedPin, OUTPUT);
  digitalWrite(badLedPin, LOW);

  Serial.begin(9600);
  Serial.println(F("=== ATS Card Checker - Arduino Nano ==="));
  Serial.println(F("Standard SPI pins: D13(SCK), D12(MISO), D11(MOSI), D10(SS)"));
  Serial.println(F("Controls: D9(Button), A0(Good LED), A1(Bad LED)"));

  Serial.println(F("\nInitializing PN532..."));
  nfc.begin();

  uint32_t versionData = nfc.getFirmwareVersion();
  
  if (!versionData) {
    // Connection FAILED
    Serial.println(F("Could not find PN532 board. Check wiring."));
    Serial.println(F("⚠️  Verify: D10→SS, D11→MOSI, D12→MISO, D13→SCK"));
    digitalWrite(badLedPin, HIGH);  // Red LED = connection failed
    delay(statusDisplayDurationMs);
    digitalWrite(badLedPin, LOW);
    
    while (1) {
      // Halt - cannot proceed without reader
    }
  }

  // Connection SUCCESS
  Serial.print(F("Found PN5"));
  Serial.println((versionData >> 24) & 0xFF, HEX);
  Serial.print(F("Firmware version: "));
  Serial.print((versionData >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versionData >> 8) & 0xFF, DEC);

  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(passiveActivationRetries);

  // Display connection success status
  digitalWrite(goodLedPin, HIGH);  // Green LED = connection successful
  Serial.println(F("Reader ready!"));
  Serial.print(F("System will be ready in "));
  Serial.print(statusDisplayDurationMs / 1000);
  Serial.println(F(" seconds..."));
  
  delay(statusDisplayDurationMs);
  digitalWrite(goodLedPin, LOW);
  
  systemReady = true;
  Serial.println(F("System ready! Press D9 button to check cards."));
  Serial.println(F("Green LED = Good card (ATS found)"));
  Serial.println(F("Red LED = Bad card (no ATS) or no card present"));
}

// ================= Main Loop =================
void loop() {
  if (!systemReady) {
    return;  // Skip loop until status display completes
  }
  
  updateTriggerState();

  if (triggerStableState == LOW && !triggerHoldReadDone) {
    // Trigger just pressed - test card
    triggerHoldReadDone = true;
    processAtsRead();
  } else if (triggerStableState == HIGH && triggerHoldReadDone) {
    // Trigger released - clear result
    triggerHoldReadDone = false;
    resetOutputs();
  }
}

// ================= Trigger State Management =================
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

// ================= ATS Detection =================
void processAtsRead() {
  unsigned long readStartMs = millis();
  Serial.println(F("\n===== Trigger pressed - Checking card ====="));

  // Re-initialize for fresh activation session
  // CRITICAL: RATS only works once per activation, so we must re-init
  // before each read to ensure RATS can be sent again
  nfc.begin();
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(passiveActivationRetries);

  // Detect card presence
  bool cardPresent = nfc.inListPassiveTarget();

  if (!cardPresent) {
    Serial.println(F("No card detected."));
    printElapsedMs(readStartMs);
    digitalWrite(badLedPin, HIGH);  // Red LED = no card
    return;
  }

  Serial.println(F("Card activated. Sending RATS..."));

  // Send RATS command to request ATS
  uint8_t atsResponse[32];
  uint8_t atsResponseLength = sizeof(atsResponse);

  bool gotAts = nfc.inDataExchange(CMD_RATS, sizeof(CMD_RATS), atsResponse, &atsResponseLength);

  if (gotAts) {
    // Good card - has ATS
    Serial.print(F("✓ GOOD CARD - ATS obtained: "));
    printHexBytes(atsResponse, atsResponseLength);
    printElapsedMs(readStartMs);
    digitalWrite(goodLedPin, HIGH);  // Green LED = good card
  } else {
    // Bad card - no ATS support
    Serial.println(F("✗ BAD CARD - No ATS (card does not support ISO14443-4)"));
    printElapsedMs(readStartMs);
    digitalWrite(badLedPin, HIGH);  // Red LED = bad card
  }
}

// ================= Output Reset =================
void resetOutputs() {
  digitalWrite(goodLedPin, LOW);
  digitalWrite(badLedPin, LOW);
}

// ================= Logging Helpers =================
void printElapsedMs(unsigned long startMs) {
  Serial.print(F("Time: "));
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
