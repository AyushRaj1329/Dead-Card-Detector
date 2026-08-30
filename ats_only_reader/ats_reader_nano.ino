/*
 * Arduino Nano - Single PN532 ATS Reader (Right Side Only)
 * 
 * This version uses ONLY the RIGHT SIDE analog pins (A0-A3)
 * Perfect for clean, organized wiring with all connections on one side!
 * 
 * Hardware Configuration:
 * - 1× PN532 NFC reader in SPI mode
 * - 1× Push button (trigger)
 * - 2× LEDs (green + red)
 * - Arduino Nano
 * 
 * Pin Assignment:
 * ================
 * SPI Bus (ICSP Header - top center):
 *   ICSP Pin 1 - MISO  (connects to PN532 MISO)
 *   ICSP Pin 2 - VCC   (connects to PN532 VCC)
 *   ICSP Pin 3 - SCK   (connects to PN532 SCK)
 *   ICSP Pin 4 - MOSI  (connects to PN532 MOSI)
 *   ICSP Pin 6 - GND   (connects to PN532 GND)
 * 
 * RIGHT SIDE ONLY (Continuous A0-A3):
 *   A0 - PN532 SS (chip select)
 *   A1 - Trigger button (INPUT_PULLUP, active LOW)
 *   A2 - Green LED (ATS found)
 *   A3 - Red LED (no ATS/no card)
 * 
 * Free Pins:
 *   A4-A7 - FREE for analog input
 *   D2-D13 - ALL FREE for digital I/O
 * 
 * Advantages:
 * - All user connections on RIGHT SIDE only (clean layout!)
 * - Continuous pins A0→A1→A2→A3 (easy to wire)
 * - Proper SS control (library compatible)
 * - Lower power consumption
 * - Expandable to 2+ readers
 * - Follows SPI best practices
 */

#include <SPI.h>
#include <Adafruit_PN532.h>

// ================= PN532 Wiring (Hardware SPI via ICSP) =================
// Using A0 for SS control - keeps all user connections on RIGHT SIDE!
// Continuous layout: A0 (SS) → A1 (Button) → A2 (Green) → A3 (Red)
const uint8_t PN532_SS = A0;  // A0 pin controls PN532 SS
Adafruit_PN532 nfc(PN532_SS);

// ================= Pin Configuration =================
// All connections on RIGHT SIDE: A0, A1, A2, A3 (continuous!)
// A4-A7 still FREE, ALL digital pins D2-D13 FREE!
const uint8_t triggerPin        = A1; // INPUT_PULLUP push button, LOW = pressed
const uint8_t outputAtsFoundPin = A2; // Green LED: ATS found / Connection successful
const uint8_t outputNoAtsPin    = A3; // Red LED: No ATS / Connection failed

// ================= Timing Configuration =================
const unsigned long statusDisplayDurationMs = 5000; // Duration to show connection status (adjustable)
const unsigned long debounceMs    = 50;
const uint8_t        passiveActivationRetries = 3;

// ================= RATS Command =================
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

  pinMode(outputAtsFoundPin, OUTPUT);
  digitalWrite(outputAtsFoundPin, LOW);
  pinMode(outputNoAtsPin, OUTPUT);
  digitalWrite(outputNoAtsPin, LOW);

  Serial.begin(9600);
  Serial.println(F("=== Arduino Nano - Single PN532 (Right Side Only) ==="));
  Serial.println(F("All connections on RIGHT side: A0-A3 continuous!"));
  
  Serial.println(F("\nInitializing PN532..."));
  nfc.begin();

  uint32_t versionData = nfc.getFirmwareVersion();
  
  if (!versionData) {
    // Connection FAILED
    Serial.println(F("Could not find PN532 board. Check wiring."));
    Serial.println(F("⚠️  Make sure A0 is connected to PN532 SS!"));
    digitalWrite(outputNoAtsPin, HIGH);
    delay(statusDisplayDurationMs);
    digitalWrite(outputNoAtsPin, LOW);
    
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
  digitalWrite(outputAtsFoundPin, HIGH);
  Serial.println(F("Reader ready. A0 controls SS pin."));
  Serial.print(F("System will be ready in "));
  Serial.print(statusDisplayDurationMs / 1000);
  Serial.println(F(" seconds..."));
  
  delay(statusDisplayDurationMs);
  digitalWrite(outputAtsFoundPin, LOW);
  
  systemReady = true;
  Serial.println(F("System ready! Press trigger to check cards."));
  Serial.println(F("Right side only: A0→A1→A2→A3 continuous layout!"));
}

// ================= Main Loop =================
void loop() {
  if (!systemReady) {
    return;
  }
  
  updateTriggerState();

  if (triggerStableState == LOW && !triggerHoldReadDone) {
    triggerHoldReadDone = true;
    processAtsRead();
  } else if (triggerStableState == HIGH && triggerHoldReadDone) {
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
  Serial.println(F("Trigger pressed. Checking for ATS..."));

  // Re-init for fresh activation
  // This resets the PN532's RF activation state
  nfc.begin();
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(passiveActivationRetries);

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
void resetOutputs() {
  digitalWrite(outputAtsFoundPin, LOW);
  digitalWrite(outputNoAtsPin, LOW);
}

// ================= Logging Helpers =================
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
