# ATS Card Checker - Arduino Nano Wiring Guide

Simple single-button NFC card tester. Press button → Green LED = Good card, Red LED = Bad card.

---

## Pin Configuration

### Standard SPI Connection (Digital Pins):

| Arduino Pin | PN532 Pin | Function |
|-------------|-----------|----------|
| **D13** | SCK | SPI Clock |
| **D12** | MISO | SPI Data In |
| **D11** | MOSI | SPI Data Out |
| **D10** | SS/SSEL | Chip Select |
| **5V** | VCC | Power (+5V) |
| **GND** | GND | Ground |

### Controls & Indicators:

| Arduino Pin | Component | Connection |
|-------------|-----------|------------|
| **D9** | Push button | Button → GND (INPUT_PULLUP) |
| **A0** | Green LED | 220Ω resistor → LED → GND |
| **A1** | Red LED | 220Ω resistor → LED → GND |

---

## Wiring Diagram

```
Arduino Nano                    PN532 Module
┌──────────┐                   ┌────────────┐
│          │                   │            │
│  D13 ────┼───────────────────┤ SCK        │
│  D12 ────┼───────────────────┤ MISO       │
│  D11 ────┼───────────────────┤ MOSI       │
│  D10 ────┼───────────────────┤ SS         │
│  5V  ────┼───────────────────┤ VCC        │
│  GND ────┼───┬───────────────┤ GND        │
│          │   │               └────────────┘
│  D9  ────┼───┼──[Button]──GND
│          │   │
│  A0  ────┼───┼──[220Ω]──(Green LED)──GND
│  A1  ────┼───┴──[220Ω]──(Red LED)──GND
│          │
└──────────┘

Legend:
[Button] = Momentary push button (normally open)
[220Ω] = 220Ω resistor (current limiting)
(LED) = LED with correct polarity (long leg = +)
```

---

## Step-by-Step Wiring

### 1. PN532 SPI Mode Configuration
Set PN532 to SPI mode (check your module's switches/jumpers):
- SW1/I2C = OFF
- SW2/SPI = ON

### 2. Power Connections
- Arduino **5V** → PN532 **VCC**
- Arduino **GND** → PN532 **GND** (and all other grounds)

### 3. SPI Bus
- Arduino **D13** → PN532 **SCK**
- Arduino **D12** → PN532 **MISO**
- Arduino **D11** → PN532 **MOSI**
- Arduino **D10** → PN532 **SS**

### 4. Trigger Button
- Arduino **D9** → Push button → **GND**
- (No resistor needed - uses internal pullup)

### 5. Good LED (Green)
- Arduino **A0** → 220Ω resistor → Green LED anode (+, long leg)
- Green LED cathode (-, short leg) → **GND**

### 6. Bad LED (Red)
- Arduino **A1** → 220Ω resistor → Red LED anode (+, long leg)
- Red LED cathode (-, short leg) → **GND**

---

## Quick Connection Checklist

- [ ] PN532 in SPI mode (switches set correctly)
- [ ] D13 → PN532 SCK
- [ ] D12 → PN532 MISO
- [ ] D11 → PN532 MOSI
- [ ] D10 → PN532 SS
- [ ] 5V → PN532 VCC
- [ ] GND → PN532 GND
- [ ] D9 → Button → GND
- [ ] A0 → 220Ω → Green LED (+) → GND
- [ ] A1 → 220Ω → Red LED (+) → GND
- [ ] LED polarity correct (long leg = +)

---

## Operation

### Power On
1. ✅ Green LED lights for 3 seconds = Connection OK
2. ❌ Red LED lights for 3 seconds = Connection failed (check wiring)
3. After 3 seconds, LED turns off → System ready

### Testing Cards
1. Place card on PN532 antenna
2. Press button (D9)
3. **Green LED** = ✓ Good card (has ATS - ISO14443-4 compatible)
4. **Red LED** = ✗ Bad card (no ATS or no card detected)
5. LED stays on while button held
6. Release button → LED turns off

---

## LED Meanings

| LED | Meaning | Example Cards |
|-----|---------|---------------|
| **Green (A0)** | Good card - ATS found | MIFARE DESFire, some bank cards, NFC payment cards |
| **Red (A1)** | Bad card - No ATS | MIFARE Classic, MIFARE Ultralight, no card present |

### At Startup:
| LED | Duration | Meaning |
|-----|----------|---------|
| Green | 3 seconds | ✅ PN532 connected successfully |
| Red | 3 seconds | ❌ PN532 not found - check wiring |

---

## Troubleshooting

### Red LED at startup (connection failed)

**Check:**
1. PN532 power (5V and GND connected)
2. PN532 in SPI mode (verify switches)
3. SPI wiring:
   - D13 → SCK (not MISO!)
   - D12 → MISO (not MOSI!)
   - D11 → MOSI (not SCK!)
   - D10 → SS
4. Try different USB cable/port

### Button doesn't work

**Check:**
1. D9 → Button → GND connections
2. Button is normally-open (NO) type
3. Button closes when pressed (test with multimeter)

### LED doesn't light

**Check:**
1. LED polarity (swap if needed - long leg = +)
2. 220Ω resistor present
3. GND connection to LED cathode
4. Correct pin (A0 = green, A1 = red)

### Card not detected

**Check:**
1. Card placed directly on PN532 antenna
2. Card is ISO14443A compatible
3. Serial Monitor shows "Card activated"
4. Try different card

---

## Serial Monitor Output

**Baud rate:** 9600

### Successful Startup:
```
=== ATS Card Checker - Arduino Nano ===
Standard SPI pins: D13(SCK), D12(MISO), D11(MOSI), D10(SS)
Controls: D9(Button), A0(Good LED), A1(Bad LED)

Initializing PN532...
Found PN532
Firmware version: 1.6
Reader ready!
System will be ready in 3 seconds...
System ready! Press D9 button to check cards.
Green LED = Good card (ATS found)
Red LED = Bad card (no ATS) or no card present
```

### Good Card Detected:
```
===== Trigger pressed - Checking card =====
Card activated. Sending RATS...
✓ GOOD CARD - ATS obtained: 05 78 80 02 00
Time: 287 ms
```

### Bad Card Detected:
```
===== Trigger pressed - Checking card =====
Card activated. Sending RATS...
✗ BAD CARD - No ATS (card does not support ISO14443-4)
Time: 256 ms
```

---

## Adjustable Settings

### Status Display Duration

In the code, change this line:
```cpp
const unsigned long statusDisplayDurationMs = 3000;  // 3 seconds
```

**Examples:**
- `1000` = 1 second
- `3000` = 3 seconds (default)
- `5000` = 5 seconds

---

## Materials Required

| Item | Quantity | Notes |
|------|----------|-------|
| Arduino Nano | 1 | Any variant |
| PN532 NFC Module | 1 | Must support SPI mode |
| Push Button | 1 | Momentary, normally-open |
| Green LED | 1 | 3mm or 5mm |
| Red LED | 1 | 3mm or 5mm |
| 220Ω Resistor | 2 | For LEDs |
| Breadboard | 1 | Half or full size |
| Jumper Wires | ~15 | Male-to-male |
| USB Cable | 1 | For programming + power |

---

## Power Requirements

| Component | Current Draw |
|-----------|--------------|
| Arduino Nano | ~50 mA |
| PN532 (active) | ~110 mA |
| 2× LEDs | ~40 mA |
| **Total** | ~200 mA |

**USB power is sufficient** ✅

---

## Summary

✅ **Simple wiring** - standard SPI pins, no ICSP needed  
✅ **One-button operation** - press to test card  
✅ **Clear indicators** - green = good, red = bad  
✅ **Startup test** - shows connection status  
✅ **Quick results** - ~300ms per card test  

**Perfect for:** Dead card detection, NFC card sorting, ATS compatibility testing

---

**File:** `ats_card_checker_nano.ino`  
**Last Updated:** 2026-08-29  
**Board:** Arduino Nano (ATmega328P)  
**Configuration:** Standard SPI (D10-D13), Single button (D9), Dual LEDs (A0-A1)
