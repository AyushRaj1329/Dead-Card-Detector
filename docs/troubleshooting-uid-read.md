# Troubleshooting: UID read works for one card type but not another

## Symptom

The firmware reliably returns a UID for one card (e.g. a "debit" card) but
reports "No UID detected" for another card (e.g. a "credit" card), even
though both are presented the same way to the reader.

## Root cause

In `dead_card_detector.ino`, a wake-up command is defined but never used:

```cpp
const uint8_t CMD_WUPA[3] = { 0x20, 0x02, 0x52 };
```

Searching the file, `CMD_WUPA` only appears on this one line — it is
never passed to `buildFrame()` or sent over the socket. `processCardRead()`
sends `CMD_INVENTORY` (`0x2F 0x01`) straight away, with no wake-up/select
step first:

```cpp
void processCardRead() {
  ...
  for (uint8_t attempt = 0; attempt < inventoryAttempts; attempt++) {
    uint8_t invFrame[8];
    uint8_t invFrameLen = buildFrame(invFrame, sizeof(invFrame), CMD_INVENTORY, 2);
    ...
```

This matters because of how ISO/IEC 14443-3 Type A cards behave:

- **REQA** wakes a card only if it's in the **IDLE** state.
- **WUPA** wakes a card whether it's in **IDLE** *or* **HALT** state.
- A card is put into HALT after it has already completed one
  select/read cycle (its own or another reader's), and stays there until
  powered off or explicitly woken with WUPA.

Many reader firmwares issue a plain REQA internally for a generic
"Inventory" command. So:

- A card that happens to still be in IDLE (fresh power-up, first tap)
  responds fine to Inventory alone. This is what you're seeing with the
  "debit" card.
- A card that has already been activated once and dropped into HALT (or
  whose chip defaults to HALT/requires an explicit wake) will **not**
  respond until a proper **WUPA** is sent first. This is what's
  happening with the "credit" card — Inventory alone isn't enough to
  bring it back to READY, so anticollision/select never happens and no
  UID comes back.

This isn't a "type of card" issue in the payment-network sense — it's
about which ISO14443 power state the chip happens to be in, which
firmware/chip design varies between cards.

## Secondary contributing factors (worth ruling out too)

1. **`inventoryTimeoutMs` (150 ms) may be too tight.** Cards requiring a
   second anticollision cascade level (7-byte UID chips) take a bit
   longer to resolve than 4-byte UID chips. Combined with TCP round-trip
   to the reader, 150 ms can be marginal.
2. **Type A only.** `CMD_WUPA`/`CMD_INVENTORY` as defined here only wake
   ISO14443 **Type A** cards. If the non-responding card is actually
   Type B, this reader command set won't activate it at all — that would
   need a different wake-up command from the reader's protocol
   reference, not a firmware fix.

Rule out #2 first if the fix below doesn't resolve it (check the
reader's documentation for its Type B command codes).

## Fix

Send `CMD_WUPA` before `CMD_INVENTORY` on each attempt, and only proceed
to Inventory if the wake-up succeeded:

```cpp
void processCardRead() {
  Serial.println(F("Trigger pressed. Reading card..."));

  for (uint8_t attempt = 0; attempt < inventoryAttempts; attempt++) {
    // Wake the card (handles cards left in HALT state, not just IDLE)
    uint8_t wupaFrame[8];
    uint8_t wupaFrameLen = buildFrame(wupaFrame, sizeof(wupaFrame), CMD_WUPA, 3);
    uint8_t wupaResp[16];
    int16_t wupaRespLen = sendCommandAndReceive(wupaFrame, wupaFrameLen, wupaResp, sizeof(wupaResp), inventoryTimeoutMs);

    if (wupaRespLen < 0) {
      Serial.println(F("Connection lost during card read."));
      disconnectAndReset();
      return;
    }

    if (isResponseOk(wupaResp, wupaRespLen)) {
      uint8_t invFrame[8];
      uint8_t invFrameLen = buildFrame(invFrame, sizeof(invFrame), CMD_INVENTORY, 2);
      uint8_t invResp[32];
      int16_t invRespLen = sendCommandAndReceive(invFrame, invFrameLen, invResp, sizeof(invResp), inventoryTimeoutMs);

      if (invRespLen < 0) {
        Serial.println(F("Connection lost during card read."));
        disconnectAndReset();
        return;
      }

      if (isResponseOk(invResp, invRespLen)) {
        uint8_t uidLen = invResp[0] - 6;
        if (uidLen > 0 && invRespLen >= 6 + uidLen) {
          Serial.print(F("UID found: "));
          printHexBytes(invResp + 6, uidLen);
          pulseOutput(outputUidFoundPin, outputUidFoundActive, outputUidFoundStartMs);
          return;
        }
      }
    }

    if (attempt + 1 < inventoryAttempts) {
      delay(inventoryRetryGapMs);
    }
  }

  Serial.println(F("No UID detected."));
  pulseOutput(outputNoUidPin, outputNoUidActive, outputNoUidStartMs);
}
```

If the timing turns out to be marginal even after adding WUPA, also try
raising `inventoryTimeoutMs` from `150` to something like `250`–`300` and
re-test with the previously-failing card.

## Suggested test procedure

1. Apply the WUPA fix above.
2. Present the previously-failing card fresh (power the field off/on by
   removing and re-presenting the card) and press the trigger — confirm
   UID now reads.
3. Present the same card a second time *without* removing it, to
   confirm it still reads correctly once it's already been selected once
   (this specifically exercises the HALT-state case).
4. Re-test the card that was already working, to confirm no regression.
