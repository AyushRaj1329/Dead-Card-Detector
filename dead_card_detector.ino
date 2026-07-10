#include <SPI.h>
#include <Ethernet.h>

// ================= Network Configuration =================
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };   // Arduino's MAC address (must be unique on the LAN)
IPAddress arduinoIP(192, 168, 1, 177);                 // Static IP assigned to the Arduino
IPAddress readerIP(192, 168, 1, 200);                  // RFID reader's IP (TCP server)
const uint16_t readerPort = 9090;                      // RFID reader's TCP port

// ================= Pin Configuration =================
const uint8_t ledStatusPin     = 6;   // Lights up while connected to the reader
const uint8_t triggerPin       = 7;   // INPUT_PULLUP push button, LOW = pressed
const uint8_t outputUidFoundPin = 8;  // Pulses HIGH when a card UID is read successfully
const uint8_t outputNoUidPin    = 9;  // Pulses HIGH when the trigger fires but no UID is found

// ================= Timing Configuration =================
const unsigned long reconnectIntervalMs  = 3000;  // Minimum time between connection attempts
const unsigned long connectTimeoutMs     = 1000;  // Max time a single connect() call is allowed to block
const unsigned long heartbeatIntervalMs  = 5000;  // How often to probe a "connected" link for real life
const unsigned long responseTimeoutMs    = 500;   // Max wait for a heartbeat command's reply
const unsigned long inventoryTimeoutMs   = 150;   // Max wait for an Inventory reply (normally arrives in a few ms)
const unsigned long debounceMs           = 50;    // Button debounce window
const unsigned long outputPulseMs        = 500;   // How long Output A / Output B stay HIGH
const uint8_t        inventoryAttempts   = 2;      // Quick retries on a trigger-driven read
const unsigned long  inventoryRetryGapMs = 20;     // Gap between retry attempts

// W5100 retransmission tuning: caps how long a single write() can block
// if the remote end has gone silent (e.g. powered off) without sending
// a proper TCP close. Default is 8 retries x 200ms = 1.6s; we tighten
// this so a dead link is detected faster.
const uint16_t retransmissionTimeoutMs = 200;
const uint8_t  retransmissionCount     = 3;

// ================= RRHFOEM04 Protocol Commands =================
// Frame format (request):  [LENGTH][CMD1][CMD2]...[DATA][CRC_HI][CRC_LO]
// Frame format (response): [LENGTH][CMD1][CMD2][STATUS_HI][STATUS_LO][DATA...][CRC_HI][CRC_LO]
// STATUS 0x00 0x00 = success. CRC is CRC-CCITT (poly 0x1021, init 0xFFFF, inverted).
const uint8_t CMD_GET_READER_INFO[2] = { 0xF0, 0x00 };  // Reader liveness probe (no RF interaction)
const uint8_t CMD_WUPA[3]            = { 0x20, 0x02, 0x52 };  // Wake card (primes the field)
const uint8_t CMD_INVENTORY[2]       = { 0x2F, 0x01 };  // Read UID of card currently on the reader

// ================= Connection State Machine =================
enum ConnState {
  CONN_DISCONNECTED,   // No active TCP connection to the reader
  CONN_CONNECTED        // TCP connection is up
};

ConnState connState = CONN_DISCONNECTED;
unsigned long lastConnectAttemptMs = 0;
unsigned long lastHeartbeatMs = 0;

// Trigger button debounce state
int  triggerRawState    = HIGH;
int  triggerStableState = HIGH;
unsigned long triggerLastChangeMs = 0;

// Output pulse state (non-blocking timers)
bool outputUidFoundActive = false;
unsigned long outputUidFoundStartMs = 0;
bool outputNoUidActive = false;
unsigned long outputNoUidStartMs = 0;

EthernetClient client;

void setup() {
  pinMode(ledStatusPin, OUTPUT);
  digitalWrite(ledStatusPin, LOW);

  pinMode(triggerPin, INPUT_PULLUP);

  pinMode(outputUidFoundPin, OUTPUT);
  digitalWrite(outputUidFoundPin, LOW);
  pinMode(outputNoUidPin, OUTPUT);
  digitalWrite(outputNoUidPin, LOW);

  Serial.begin(9600);

  Ethernet.begin(mac, arduinoIP);

  Serial.println(F("Ethernet Initialized"));
  Serial.print(F("Arduino IP: "));
  Serial.println(Ethernet.localIP());

  client.setConnectionTimeout(connectTimeoutMs);
  Ethernet.setRetransmissionTimeout(retransmissionTimeoutMs);
  Ethernet.setRetransmissionCount(retransmissionCount);

  // Force the very first loop() pass to attempt a connection immediately
  // instead of waiting out the first reconnectIntervalMs.
  lastConnectAttemptMs = millis() - reconnectIntervalMs;
}

void loop() {
  // Output pulse timers run every pass, independent of connection state,
  // so a pulse always completes on schedule even if the link drops mid-pulse.
  serviceOutputPulses();

  switch (connState) {
    case CONN_DISCONNECTED:
      handleDisconnected();
      break;

    case CONN_CONNECTED:
      handleConnected();
      break;
  }
}

// ================= Connection Handling =================

// Attempts a new connection, but only once every reconnectIntervalMs.
void handleDisconnected() {
  digitalWrite(ledStatusPin, LOW);

  unsigned long now = millis();
  if (now - lastConnectAttemptMs < reconnectIntervalMs) {
    return;  // Not time to retry yet; leave the CPU free for other work
  }
  lastConnectAttemptMs = now;

  Serial.println(F("Attempting to connect to RFID reader..."));

  if (client.connect(readerIP, readerPort)) {
    Serial.println(F("Connected."));
    connState = CONN_CONNECTED;
    digitalWrite(ledStatusPin, HIGH);
    lastHeartbeatMs = now;  // don't fire a heartbeat the instant we connect
  } else {
    Serial.println(F("Connect failed. Will retry."));
    client.stop();  // Release the W5100 hardware socket so it can be reused
  }
}

// While connected: watches for a dead socket, runs the periodic heartbeat,
// and services the trigger button. Heartbeat and trigger both use the same
// blocking send/receive helper below -- only one command is ever in flight
// on the socket at a time, so responses can never be confused with each other.
void handleConnected() {
  if (!client.connected()) {
    Serial.println(F("Connection lost (socket closed)."));
    disconnectAndReset();
    return;
  }

  if (checkTriggerPressed()) {
    processCardRead();
    return;  // Prioritize the read result; heartbeat resumes next pass.
  }

  unsigned long now = millis();
  if (now - lastHeartbeatMs >= heartbeatIntervalMs) {
    lastHeartbeatMs = now;
    doHeartbeat();
  }
}

// Shared cleanup used whenever we decide the connection is dead.
void disconnectAndReset() {
  client.stop();
  connState = CONN_DISCONNECTED;
  digitalWrite(ledStatusPin, LOW);
}

// ================= Heartbeat =================

// Sends "Get Reader Information" (0xF0 0x00). This command never touches
// the RF field or queries for a card -- it purely asks the reader "are you
// there and responding." That keeps it completely decoupled from the
// trigger-driven Inventory read, so the heartbeat can run in the background
// on its own timer without ever interfering with card detection.
void doHeartbeat() {
  uint8_t frame[8];
  uint8_t frameLen = buildFrame(frame, sizeof(frame), CMD_GET_READER_INFO, 2);

  uint8_t resp[16];
  int16_t respLen = sendCommandAndReceive(frame, frameLen, resp, sizeof(resp), responseTimeoutMs);

  if (respLen < 0) {
    Serial.println(F("Connection lost (heartbeat write failed)."));
    disconnectAndReset();
    return;
  }

  if (respLen == 0) {
    // A healthy reader always answers Get Reader Info promptly.
    // Silence here means the link is dead even though connected() didn't notice.
    Serial.println(F("Connection lost (heartbeat: no response)."));
    disconnectAndReset();
    return;
  }

  if (!isResponseOk(resp, respLen)) {
    // Reader replied but reported an error status. The link itself is fine;
    // this is not treated as a disconnect.
    Serial.println(F("Heartbeat: reader responded with error status."));
  }
}

// ================= Trigger-Driven Card Read =================

// Debounced edge detector: returns true exactly once per physical button press.
bool checkTriggerPressed() {
  int current = digitalRead(triggerPin);
  unsigned long now = millis();

  if (current != triggerRawState) {
    triggerRawState = current;
    triggerLastChangeMs = now;
  }

  if (now - triggerLastChangeMs > debounceMs && triggerStableState != triggerRawState) {
    triggerStableState = triggerRawState;
    if (triggerStableState == LOW) {
      return true;  // Falling edge = press event (INPUT_PULLUP: LOW = pressed)
    }
  }

  return false;
}

// Runs the Inventory command to read the UID of whatever card is already
// sitting on the reader, then drives Output A or Output B based on the
// result. A couple of quick retries guard against a single dropped frame --
// the card is expected to already be in place, so this is not a "wait for
// the operator" loop.
//
// NOTE: WUPA (0x20 0x02 0x52) is intentionally NOT sent here. On this
// reader, WUPA reliably returns an "RF not active" error frame whose
// LENGTH byte never satisfies our frame-completion check, which forced
// every trigger press to sit through the full responseTimeoutMs before
// moving on. Inventory alone already activates the card -- confirmed by
// nfc.py's card_inventory(), the reference driver's production path,
// which sends only 0x2F 0x01 with no WUPA step.
void processCardRead() {
  Serial.println(F("Trigger pressed. Reading card..."));

  for (uint8_t attempt = 0; attempt < inventoryAttempts; attempt++) {
    uint8_t invFrame[8];
    uint8_t invFrameLen = buildFrame(invFrame, sizeof(invFrame), CMD_INVENTORY, 2);
    uint8_t invResp[32];
    int16_t invRespLen = sendCommandAndReceive(invFrame, invFrameLen, invResp, sizeof(invResp), inventoryTimeoutMs);

    if (invRespLen < 0) {
      // The write itself failed -- this is a dead connection, not a "no card"
      // result. We don't know the answer, so neither output is driven.
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

    if (attempt + 1 < inventoryAttempts) {
      delay(inventoryRetryGapMs);
    }
  }

  Serial.println(F("No UID detected."));
  pulseOutput(outputNoUidPin, outputNoUidActive, outputNoUidStartMs);
}

// ================= Output Pulse Handling =================

void pulseOutput(uint8_t pin, bool &activeFlag, unsigned long &startMs) {
  digitalWrite(pin, HIGH);
  activeFlag = true;
  startMs = millis();
}

// Turns off any output pin whose 500ms pulse window has elapsed.
// Runs every loop() pass so a pulse always completes on schedule.
void serviceOutputPulses() {
  unsigned long now = millis();

  if (outputUidFoundActive && now - outputUidFoundStartMs >= outputPulseMs) {
    digitalWrite(outputUidFoundPin, LOW);
    outputUidFoundActive = false;
  }

  if (outputNoUidActive && now - outputNoUidStartMs >= outputPulseMs) {
    digitalWrite(outputNoUidPin, LOW);
    outputNoUidActive = false;
  }
}

// ================= RRHFOEM04 Protocol Helpers =================

// CRC-CCITT: poly 0x1021, init 0xFFFF, result bitwise-inverted.
// Direct port of the reference driver's calc_crc(), byte-for-byte identical logic.
uint16_t calcCrc(const uint8_t* data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc = crc << 1;
      }
    }
  }
  return ~crc;
}

// Builds a complete request frame: [LENGTH][cmdBytes...][CRC_HI][CRC_LO].
// outFrame must have at least cmdLen + 3 bytes of space.
// Returns the total frame length actually written.
uint8_t buildFrame(uint8_t* outFrame, uint8_t outFrameCap, const uint8_t* cmdBytes, uint8_t cmdLen) {
  if (cmdLen + 3 > outFrameCap) {
    return 0;  // Frame would not fit -- caller passed too small a buffer
  }

  uint8_t length = cmdLen + 1;
  outFrame[0] = length;
  for (uint8_t i = 0; i < cmdLen; i++) {
    outFrame[1 + i] = cmdBytes[i];
  }

  uint16_t crc = calcCrc(outFrame, 1 + cmdLen);
  outFrame[1 + cmdLen] = (crc >> 8) & 0xFF;
  outFrame[2 + cmdLen] = crc & 0xFF;

  return cmdLen + 3;
}

// Sends one frame and blocks (bounded by timeoutMs) for the response.
// Returns:
//   -1            write failed outright -- treat as a dead connection
//    0            no response arrived within timeoutMs
//   >0            number of response bytes received (may be a full or
//                 partial frame; caller must validate with isResponseOk)
int16_t sendCommandAndReceive(const uint8_t* frame, uint8_t frameLen,
                               uint8_t* respBuf, uint8_t respCap,
                               unsigned long timeoutMs) {
  // Discard any bytes left over from a previous exchange (e.g. a heartbeat
  // reply that arrived just after we gave up waiting for it). Without this,
  // those stale bytes get read as if they belonged to THIS command's
  // response, silently corrupting the result.
  drainSocket();

  size_t written = client.write(frame, frameLen);
  if (written != frameLen) {
    return -1;
  }

  uint8_t received = 0;
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    while (client.available() && received < respCap) {
      respBuf[received++] = (uint8_t)client.read();
    }

    // Same completion check as the reference driver's exchange():
    // a full frame has arrived once LENGTH (respBuf[0]) plus the two
    // trailing CRC bytes fits within what we've received so far.
    if (received >= 3 && respBuf[0] <= received - 2) {
      return received;
    }
  }

  return received;  // 0 if nothing arrived, or a partial frame on timeout
}

// Reads and discards any bytes currently sitting unread in the socket
// buffer, without blocking. Used before every command send to guarantee
// we never mistake a stale reply for the one we're about to request.
void drainSocket() {
  while (client.available()) {
    client.read();
  }
}

// True if the response is at least long enough to hold a status field
// and reports success (STATUS bytes both 0x00).
bool isResponseOk(const uint8_t* resp, int16_t respLen) {
  return respLen >= 5 && resp[3] == 0x00 && resp[4] == 0x00;
}

void printHexBytes(const uint8_t* data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}
