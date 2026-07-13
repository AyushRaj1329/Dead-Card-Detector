#include <SPI.h>
#include <Ethernet.h>

// ================= Network Configuration =================
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress arduinoIP(192, 168, 1, 177);
IPAddress readerIP(192, 168, 1, 200);
const uint16_t readerPort = 9090;

// ================= Pin Configuration =================
const uint8_t ledStatusPin      = 6;   // Connection status LED
const uint8_t triggerPin        = 7;   // INPUT_PULLUP push button, LOW = pressed
const uint8_t outputUidFoundPin = 8;   // Pulses HIGH when a UID is read successfully
const uint8_t outputNoUidPin    = 9;   // Pulses HIGH when no UID is found

// ================= Timing Configuration =================
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

// ================= Reader Protocol Commands =================
const uint8_t CMD_GET_READER_INFO[2] = { 0xF0, 0x00 };
const uint8_t CMD_WUPA[3]            = { 0x20, 0x02, 0x52 };
const uint8_t CMD_INVENTORY[2]       = { 0x2F, 0x01 };

// ================= Connection State Machine =================
enum ConnState {
  CONN_DISCONNECTED,
  CONN_CONNECTED
};

ConnState connState = CONN_DISCONNECTED;
unsigned long lastConnectAttemptMs = 0;
unsigned long lastHeartbeatMs = 0;

int  triggerRawState    = HIGH;
int  triggerStableState = HIGH;
unsigned long triggerLastChangeMs = 0;

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

  lastConnectAttemptMs = millis() - reconnectIntervalMs;
}

void loop() {
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

void handleDisconnected() {
  digitalWrite(ledStatusPin, LOW);

  unsigned long now = millis();
  if (now - lastConnectAttemptMs < reconnectIntervalMs) {
    return;
  }
  lastConnectAttemptMs = now;

  Serial.println(F("Attempting to connect to RFID reader..."));

  if (client.connect(readerIP, readerPort)) {
    Serial.println(F("Connected."));
    connState = CONN_CONNECTED;
    digitalWrite(ledStatusPin, HIGH);
    lastHeartbeatMs = now;
  } else {
    Serial.println(F("Connect failed. Will retry."));
    client.stop();
  }
}

void handleConnected() {
  if (!client.connected()) {
    Serial.println(F("Connection lost (socket closed)."));
    disconnectAndReset();
    return;
  }

  if (checkTriggerPressed()) {
    processCardRead();
    return;
  }

  unsigned long now = millis();
  if (now - lastHeartbeatMs >= heartbeatIntervalMs) {
    lastHeartbeatMs = now;
    doHeartbeat();
  }
}

void disconnectAndReset() {
  client.stop();
  connState = CONN_DISCONNECTED;
  digitalWrite(ledStatusPin, LOW);
}

// ================= Heartbeat =================

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
    Serial.println(F("Connection lost (heartbeat: no response)."));
    disconnectAndReset();
    return;
  }

  if (!isResponseOk(resp, respLen)) {
    Serial.println(F("Heartbeat: reader responded with error status."));
  }
}

// ================= Trigger-Driven Card Read =================

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
      return true;
    }
  }

  return false;
}

void processCardRead() {
  Serial.println(F("Trigger pressed. Reading card..."));

  for (uint8_t attempt = 0; attempt < inventoryAttempts; attempt++) {
    // Wake the card first. WUPA (unlike a plain REQA/Inventory) also
    // wakes cards left in the HALT state after a prior select cycle,
    // which is why some cards would otherwise fail to respond.
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

// ================= Output Pulse Handling =================

void pulseOutput(uint8_t pin, bool &activeFlag, unsigned long &startMs) {
  digitalWrite(pin, HIGH);
  activeFlag = true;
  startMs = millis();
}

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

// ================= Reader Protocol Helpers =================

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

uint8_t buildFrame(uint8_t* outFrame, uint8_t outFrameCap, const uint8_t* cmdBytes, uint8_t cmdLen) {
  if (cmdLen + 3 > outFrameCap) {
    return 0;
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

int16_t sendCommandAndReceive(const uint8_t* frame, uint8_t frameLen,
                               uint8_t* respBuf, uint8_t respCap,
                               unsigned long timeoutMs) {
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

    if (received >= 3 && respBuf[0] <= received - 2) {
      return received;
    }
  }

  return received;
}

void drainSocket() {
  while (client.available()) {
    client.read();
  }
}

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
