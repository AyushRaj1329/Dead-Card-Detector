# Code Explained: `dead_card_detector.ino` (superseded)

> **⚠️ Historical document — does not describe the current firmware.**
>
> This walks through `dead_card_detector.ino`, the original Ethernet/TCP
> sketch that drove an RRHFOEM04 reader and detected cards by **UID**. That
> sketch was **deleted** from the repo when the project moved to a PN532
> reader with **ATS**-based detection.
>
> Nothing here applies to the current hardware or firmware: there is no
> Ethernet shield, no TCP connection, no heartbeat, no CRC framing, and no
> UID reading in the project any more.
>
> - **Current firmware:** [`../ats_only_reader/ats_only_reader.ino`](../ats_only_reader/ats_only_reader.ino)
> - **Current walkthrough:** [`../ats_only_reader/ats_only_reader_explained.md`](../ats_only_reader/ats_only_reader_explained.md)
> - **Why it changed:** [`ats-based-detection-proposal.md`](ats-based-detection-proposal.md)
>
> Kept for reference: the CRC/framing and reader-protocol notes below are
> still an accurate record of how that reader's protocol worked.

This document walks through the entire sketch section by section, explains
the terminology used (RFID/networking jargon), and traces the control flow
from boot to a single card read.

## 1. High-level picture

The Arduino is a **TCP client**. It connects over Ethernet to an RFID
reader (which runs as a **TCP server**), and talks to it using a small
binary command/response protocol. The Arduino itself never talks RF
directly to the card — the reader hardware does that. The Arduino's job is:

1. Keep a live TCP connection to the reader, reconnecting automatically.
2. Periodically confirm the reader is still alive (heartbeat).
3. On a button press, ask the reader "is there a card there, and what's
   its UID?"
4. Light one of two output pins depending on the answer.

```
[Button] --> [Arduino: this sketch] <--TCP/Ethernet--> [RFID Reader] <--RF--> [Card]
                    |
                    +--> [Status LED]
                    +--> [UID Found output]
                    +--> [No UID output]
```

## 2. Terminology

| Term | Meaning |
|---|---|
| **UID** | Unique ID burned into an RFID/NFC card's chip. Reading it is the whole point of this project. |
| **ISO14443 Type A** | The contactless card standard this reader's commands target (the command bytes used here — WUPA, Inventory — are Type A specific). |
| **REQA / WUPA** | "Request/Wake-Up Type A" — RF-level commands that bring a card out of a low-power state so it can respond. WUPA wakes a card from *any* state (including HALT); a plain REQA/Inventory only reliably wakes a card from IDLE. See `docs/troubleshooting-uid-read.md` for why this distinction matters here. |
| **HALT state** | A card enters this state after it's already been through one full select cycle. It stops responding to inventory/REQA until woken again. |
| **Inventory** | The reader command that runs anticollision + select and returns whatever card UID it finds in the field. |
| **Anticollision** | The RF-level process the reader uses to isolate one card's UID when multiple cards might be present. Not implemented at the Arduino level — it's handled inside the reader, and only the result comes back over TCP. |
| **CRC** | Cyclic Redundancy Check — a checksum appended to each command frame so the reader can detect corrupted messages. Computed with `calcCrc()`. |
| **Frame** | The full byte sequence sent to the reader for one command: `[length][command bytes...][CRC high][CRC low]`. Built by `buildFrame()`. |
| **Heartbeat** | A periodic "are you still there?" check sent to the reader independent of any card read, used to detect a dead reader before the user tries to scan a card. |
| **Debounce** | Filtering out the electrical noise/bounce of a mechanical button press so one physical press is registered as exactly one logical press. |
| **Non-blocking / `millis()`-based timing** | Instead of `delay()` (which freezes the whole program), the code repeatedly checks `millis() - lastEventTime >= interval` so it can keep doing other work (servicing outputs, checking the button) while "waiting." |
| **W5100** | The Ethernet controller chip on the Arduino Ethernet Shield, driven by the `Ethernet.h` library. |
| **`EthernetClient`** | The Arduino library class representing one outgoing TCP connection — `client` in this code is the single persistent connection to the reader. |

## 3. Section-by-section walkthrough

### 3.1 Includes and network configuration

```cpp
#include <SPI.h>
#include <Ethernet.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress arduinoIP(192, 168, 1, 177);
IPAddress readerIP(192, 168, 1, 200);
const uint16_t readerPort = 9090;
```

- `SPI.h` — the Ethernet shield talks to the Arduino over SPI; this is a
  dependency of `Ethernet.h`.
- `mac[]` — a MAC address the shield presents on the network. It's
  arbitrary/hardcoded here since this is a private, fixed-topology
  network (no DHCP, no other devices to collide with).
- `arduinoIP` — static IP the Arduino assigns itself (`Ethernet.begin`
  below uses this directly instead of DHCP, so there's no boot delay
  waiting for a lease and no dependency on a DHCP server being present).
- `readerIP` / `readerPort` — where the reader's TCP server is listening.

### 3.2 Pin configuration

```cpp
const uint8_t ledStatusPin      = 6;
const uint8_t triggerPin        = 7;
const uint8_t outputUidFoundPin = 8;
const uint8_t outputNoUidPin    = 9;
```

Four physical I/O lines:
- **Pin 6 (output)** — lit while the Arduino has a live connection to the reader.
- **Pin 7 (input)** — the trigger button, wired to GND, read with
  `INPUT_PULLUP` so the pin idles HIGH and reads LOW when pressed.
- **Pin 8 (output)** — pulses HIGH for `outputPulseMs` when a UID was
  successfully read.
- **Pin 9 (output)** — pulses HIGH for `outputPulseMs` when no UID was found.

### 3.3 Timing configuration

```cpp
const unsigned long reconnectIntervalMs  = 3000;
const unsigned long connectTimeoutMs     = 1000;
const unsigned long heartbeatIntervalMs  = 5000;
const unsigned long responseTimeoutMs    = 500;
const unsigned long inventoryTimeoutMs   = 150;
const unsigned long debounceMs           = 50;
const unsigned long outputPulseMs        = 500;
const uint8_t        inventoryAttempts   = 2;
const unsigned long  inventoryRetryGapMs = 20;

const uint16_t retransmissionTimeoutMs = 200;
const uint8_t  retransmissionCount     = 3;
```

Every timing/behavior knob in the sketch lives here, so tuning it never
requires touching logic elsewhere:

- `reconnectIntervalMs` — how often to retry connecting to the reader
  while disconnected.
- `connectTimeoutMs` — how long a single `client.connect()` attempt is
  allowed to take.
- `heartbeatIntervalMs` — how often, while connected and idle, to check
  the reader is still responsive.
- `responseTimeoutMs` — how long to wait for a reply to the heartbeat
  command specifically.
- `inventoryTimeoutMs` — how long to wait for a reply to a WUPA or
  Inventory command during a card read (shorter than the heartbeat
  timeout because the user is actively waiting on this one).
- `debounceMs` — minimum time the button's electrical state must stay
  stable before a state change is treated as real.
- `outputPulseMs` — how long the result LEDs/relay outputs stay HIGH
  after a read.
- `inventoryAttempts` — how many WUPA+Inventory cycles to try per button
  press before giving up.
- `inventoryRetryGapMs` — pause between those attempts.
- `retransmissionTimeoutMs` / `retransmissionCount` — passed straight to
  the W5100's own TCP retransmission logic (`Ethernet.setRetransmission*`),
  controlling how the underlying TCP stack retries lost packets.

### 3.4 Reader protocol commands

```cpp
const uint8_t CMD_GET_READER_INFO[2] = { 0xF0, 0x00 };
const uint8_t CMD_WUPA[3]            = { 0x20, 0x02, 0x52 };
const uint8_t CMD_INVENTORY[2]       = { 0x2F, 0x01 };
```

Raw command bytes for this specific reader's protocol (vendor-defined —
not a generic standard). `buildFrame()` wraps whichever of these is used
into a full frame with a length byte and CRC. `CMD_GET_READER_INFO` is
used for the heartbeat; `CMD_WUPA` and `CMD_INVENTORY` are used together
during a card read (see §3.8).

### 3.5 Connection state machine

```cpp
enum ConnState { CONN_DISCONNECTED, CONN_CONNECTED };
ConnState connState = CONN_DISCONNECTED;
```

The whole sketch's behavior pivots on exactly one variable, `connState`.
Everything else (heartbeat, card reads) only runs while `CONN_CONNECTED`;
everything else resets to `CONN_DISCONNECTED` the moment anything goes
wrong. This single-source-of-truth design means there's only one place
("disconnectAndReset()") that ever needs to decide "we are no longer
connected."

### 3.6 `setup()`

Runs once at boot:
1. Configures all four pins (status LED and outputs as `OUTPUT` and
   driven LOW initially; trigger as `INPUT_PULLUP`).
2. Starts `Serial` at 9600 baud for debug logging.
3. Brings up Ethernet with the static IP and prints it for confirmation.
4. Configures the client's connect timeout and the shield's TCP
   retransmission behavior.
5. Backdates `lastConnectAttemptMs` by a full `reconnectIntervalMs` so
   the very first connection attempt in `loop()` happens immediately
   rather than waiting out the interval once at boot.

### 3.7 `loop()` and the state machine

```cpp
void loop() {
  serviceOutputPulses();
  switch (connState) {
    case CONN_DISCONNECTED: handleDisconnected(); break;
    case CONN_CONNECTED:    handleConnected();    break;
  }
}
```

Every single pass through `loop()`:
1. `serviceOutputPulses()` checks if either result LED's pulse duration
   has elapsed and turns it off if so. This runs unconditionally, every
   loop, regardless of connection state — pulses always time out on
   schedule even if the connection drops mid-pulse.
2. Dispatches to whichever handler matches the current connection state.

**`handleDisconnected()`**: turns the status LED off, and — no more often
than every `reconnectIntervalMs` — attempts `client.connect(readerIP,
readerPort)`. On success, flips to `CONN_CONNECTED`, lights the status
LED, and stamps `lastHeartbeatMs` so the first heartbeat is scheduled a
full interval out. On failure, calls `client.stop()` (releases the socket
resource) and stays disconnected to retry next interval.

**`handleConnected()`**: first checks `client.connected()` — this
detects the reader having closed the socket from its end even if the
Arduino sent nothing. If so, disconnects and resets. Otherwise: if the
debounced trigger just fired, do a card read and return immediately
(skipping the heartbeat check that pass). Otherwise, if it's been
`heartbeatIntervalMs` since the last heartbeat, send one.

This ordering matters: a card read always takes priority over a
heartbeat in the same loop iteration, so pressing the button is never
delayed by a heartbeat that happens to be due at the same instant.

### 3.8 Card read flow (`checkTriggerPressed` + `processCardRead`)

**Debouncing** (`checkTriggerPressed`): every loop, reads the raw pin
state. If it differs from the last-seen raw state, records the time of
that change. Only once the raw state has held steady for longer than
`debounceMs` does the "stable" state actually update — and only a
transition of the *stable* state to LOW is reported as a press. This is
a standard software debounce: it ignores the several milliseconds of
electrical bounce a mechanical switch produces and reports one clean
edge per physical press.

**Reading the card** (`processCardRead`), on a detected press:

```
for attempt in 1..inventoryAttempts:
    send WUPA, wait up to inventoryTimeoutMs
    if WUPA got an OK response:
        send INVENTORY, wait up to inventoryTimeoutMs
        if INVENTORY got an OK response and a UID payload:
            print UID, pulse "UID found" output, done
    if not last attempt: wait inventoryRetryGapMs, retry
if all attempts exhausted:
    pulse "no UID" output
```

Two commands per attempt, in order:
1. **WUPA** — wakes the card regardless of whether it was idle or
   halted from a previous read (see terminology table above and
   `docs/troubleshooting-uid-read.md` for why this step exists).
2. **INVENTORY** — only sent if WUPA succeeded; asks the reader to run
   anticollision/select and hand back whatever UID it found.

The UID length is derived from the response's own length byte:
`uidLen = invResp[0] - 6` (the response's declared length minus the
fixed 6 bytes of header/status/CRC overhead that precede the UID
payload — see §3.9 for the exact byte layout). The read is only accepted
as successful if `uidLen > 0` and the buffer actually contains that many
bytes (`invRespLen >= 6 + uidLen`), guarding against a truncated/garbled
response being treated as a valid short UID.

If either command's send fails at the TCP layer (`sendCommandAndReceive`
returns a negative length, meaning the socket write itself failed), the
function assumes the connection is dead, calls `disconnectAndReset()`,
and returns immediately — it does not fall through to reporting "no
UID," because "the network is down" and "the card didn't respond" are
deliberately treated as different outcomes (only the latter should ever
light the "no UID" output).

### 3.9 Output pulse handling

```cpp
void pulseOutput(uint8_t pin, bool &activeFlag, unsigned long &startMs) {
  digitalWrite(pin, HIGH);
  activeFlag = true;
  startMs = millis();
}
```

Starts a pulse: drive the pin HIGH immediately, and remember when so
`serviceOutputPulses()` (called every `loop()`) can turn it back off once
`outputPulseMs` has elapsed. Passing `activeFlag` and `startMs` by
reference lets one small function work for either of the two output
pins/flag-pairs without duplicating the logic.

### 3.10 Reader protocol helpers

**`calcCrc()`** — implements CRC-16/CCITT-FALSE-style checksum
(polynomial `0x1021`, initial value `0xFFFF`, final XOR via the `~crc`
at the end) over the frame's length+command bytes. This must match
whatever CRC variant the reader itself expects; it's computed once per
outgoing frame in `buildFrame()`.

**`buildFrame()`** — assembles a complete outgoing frame:

```
byte 0:       length = cmdLen + 1
bytes 1..N:   command bytes
byte N+1:     CRC high byte
byte N+2:     CRC low byte
```

Returns the total frame length, or `0` if the destination buffer isn't
big enough for `cmdLen + 3` bytes.

**`sendCommandAndReceive()`** — the core request/response primitive used
by every command in the sketch (heartbeat, WUPA, inventory):
1. `drainSocket()` first, discarding any stale bytes left over from a
   previous exchange so they can't be misread as this response.
2. Writes the frame; if the byte count written doesn't match the frame
   length, returns `-1` (signals a dead connection to the caller).
3. Polls `client.available()` in a loop bounded by `timeoutMs`,
   accumulating bytes into `respBuf`.
4. Considers the response complete once at least 3 bytes have arrived
   and the declared length byte (`respBuf[0]`) is consistent with the
   number of bytes received so far (`respBuf[0] <= received - 2`) —
   i.e. as soon as a full frame has arrived, it returns without waiting
   out the rest of the timeout.
5. If the timeout expires first, returns however many bytes did arrive
   (possibly 0), letting the caller decide how to handle a partial or
   empty response.

**`drainSocket()`** — reads and discards any bytes currently sitting in
the receive buffer. Used before sending a new command so a slow/stale
response to a *previous* command can never be mistaken for the response
to the one about to be sent.

**`isResponseOk()`** — a response is considered successful only if it's
at least 5 bytes long and bytes 3 and 4 (the reader's status code field)
are both `0x00`. This is checked identically after the heartbeat, WUPA,
and inventory commands.

**`printHexBytes()`** — debug helper that prints a byte array as
space-separated two-digit hex (zero-padded), used to log the UID to
Serial.

## 4. Response byte layout (as used by this code)

Based on how the response buffer is indexed elsewhere in the file, a
reader response frame is laid out as:

```
byte 0:      declared length of the rest of the frame
bytes 1-2:   (reader-specific fields, not inspected by this code)
byte 3:      status byte (high) — must be 0x00 for isResponseOk()
byte 4:      status byte (low)  — must be 0x00 for isResponseOk()
byte 5:      (reserved/reader-specific)
bytes 6..N:  UID payload (only present/meaningful on an Inventory response)
```

This layout is inferred entirely from the indexing in `isResponseOk()`
(`resp[3]`, `resp[4]`) and `processCardRead()` (`invResp[0] - 6`,
`invResp + 6`) — it isn't independently documented elsewhere in this
repo, so treat it as reader-specific and verify against your reader's
actual protocol reference if you need to extend this further (e.g. to
read tag memory, not just the UID).

## 5. Summary of the full lifecycle

1. Boot → `setup()` brings up Ethernet, sets pin modes, primes timers.
2. `loop()` spins continuously:
   - Result-pulse timeouts are serviced every pass, unconditionally.
   - While disconnected: retry connecting every 3 s.
   - While connected: watch for a debounced button press (triggers a
     card read) and otherwise send a heartbeat every 5 s.
3. A card read: WUPA to wake the card, then Inventory to get its UID,
   retried up to `inventoryAttempts` times; success pulses pin 8, total
   failure pulses pin 9.
4. Any TCP-level failure (write fails, socket closes, no/garbled
   heartbeat response) drops back to the disconnected state, and the
   reconnect loop takes over with no manual intervention required.
