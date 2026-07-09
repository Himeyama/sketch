#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// 各ピンはMOSFETのゲートを駆動し、1チャンネルあたりLED4個並列を切り替える
const int NUM_CHANNELS = 9;
const int channelPins[NUM_CHANNELS] = {23, 22, 21, 19, 18, 5, 17, 16, 4};
const int connectionLedPin = 2;

#define SERVICE_UUID "a2f3b100-2cde-4f36-9e2a-3b6a2e8f9c11"
#define MODE_CHARACTERISTIC_UUID "a2f3b101-2cde-4f36-9e2a-3b6a2e8f9c11"

const int NUM_MODES = 5; // 0: stopped, 1: chase, 2: blink all, 3: alternate, 4: all on
int mode = 0;
int chaseIndex = 0;
bool blinkState = false;
unsigned long lastUpdate = 0;

const unsigned long CHASE_INTERVAL = 80;
const unsigned long BLINK_INTERVAL = 200;
const unsigned long ALT_INTERVAL = 200;

BLECharacteristic *modeCharacteristic;
BLEAdvertising *advertising;

void allOff() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    digitalWrite(channelPins[i], LOW);
  }
}

void resetModeState() {
  allOff();
  chaseIndex = 0;
  blinkState = false;
  lastUpdate = millis();
}

// 呼び出し側が 0 <= newMode < NUM_MODES を保証する
void setMode(int newMode) {
  mode = newMode;
  resetModeState();
}

void chaseMode() {
  if (millis() - lastUpdate >= CHASE_INTERVAL) {
    lastUpdate = millis();
    allOff();
    digitalWrite(channelPins[NUM_CHANNELS - 1 - chaseIndex], HIGH);
    chaseIndex = (chaseIndex + 1) % NUM_CHANNELS;
  }
}

void blinkAllMode() {
  if (millis() - lastUpdate >= BLINK_INTERVAL) {
    lastUpdate = millis();
    blinkState = !blinkState;
    for (int i = 0; i < NUM_CHANNELS; i++) {
      digitalWrite(channelPins[i], blinkState ? HIGH : LOW);
    }
  }
}

void alternateMode() {
  if (millis() - lastUpdate >= ALT_INTERVAL) {
    lastUpdate = millis();
    blinkState = !blinkState;
    for (int i = 0; i < NUM_CHANNELS; i++) {
      bool isEven = (i % 2 == 0);
      digitalWrite(channelPins[i], (isEven == blinkState) ? HIGH : LOW);
    }
  }
}

void allOnMode() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    digitalWrite(channelPins[i], HIGH);
  }
}

class ModeCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if (value.length() == 0) return;
    char c = value[0];
    if (c >= '0' && c < '0' + NUM_MODES) {
      setMode(c - '0');
    }
  }
};

class ConnectionCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    digitalWrite(connectionLedPin, HIGH);
  }

  void onDisconnect(BLEServer *server) override {
    digitalWrite(connectionLedPin, LOW);
    setMode(0);
    modeCharacteristic->setValue("0");
    advertising->start();
  }
};

void setup() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(channelPins[i], OUTPUT);
  }
  pinMode(connectionLedPin, OUTPUT);
  digitalWrite(connectionLedPin, LOW);
  allOff();

  BLEDevice::init("ESP32-LEDPanel");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ConnectionCallbacks());

  BLEService *service = server->createService(SERVICE_UUID);
  modeCharacteristic = service->createCharacteristic(
      MODE_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  modeCharacteristic->setCallbacks(new ModeCallbacks());
  modeCharacteristic->setValue("0");
  service->start();

  advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->start();
}

void loop() {
  switch (mode) {
    case 0:
      // stopped: LEDs stay off
      break;
    case 1:
      chaseMode();
      break;
    case 2:
      blinkAllMode();
      break;
    case 3:
      alternateMode();
      break;
    case 4:
      allOnMode();
      break;
  }
}
