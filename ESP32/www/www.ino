#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

// BLE 接続中に点灯する。多くの devkit ではオンボード LED も GPIO2 に繋がっている
const int connectionLedPin = 2;

#define SERVICE_UUID "7d2ec300-4a9b-4d6e-9f31-08c7a5be1d40"
#define AUTH_CHARACTERISTIC_UUID "7d2ec301-4a9b-4d6e-9f31-08c7a5be1d40"
#define SSID_CHARACTERISTIC_UUID "7d2ec302-4a9b-4d6e-9f31-08c7a5be1d40"
#define PASSWORD_CHARACTERISTIC_UUID "7d2ec303-4a9b-4d6e-9f31-08c7a5be1d40"
#define CONTROL_CHARACTERISTIC_UUID "7d2ec304-4a9b-4d6e-9f31-08c7a5be1d40"
#define STATUS_CHARACTERISTIC_UUID "7d2ec305-4a9b-4d6e-9f31-08c7a5be1d40"
#define SCAN_CHARACTERISTIC_UUID "7d2ec306-4a9b-4d6e-9f31-08c7a5be1d40"

const char *PREFERENCES_NAMESPACE = "wwwcfg";
const char *DEFAULT_ADMIN_PASSWORD = "esp32admin";

const unsigned long WIFI_CONNECT_TIMEOUT = 20000;
const int MAX_SCAN_RESULTS = 12;
// GATT の 1 属性は 512 バイトまで。JSON 組み立て中に超えないよう余裕を持たせる
const size_t SCAN_JSON_LIMIT = 480;

enum class WifiState { Idle, Scanning, Connecting, Connected, Failed };

BLECharacteristic *statusCharacteristic;
BLECharacteristic *scanCharacteristic;
BLEAdvertising *advertising;
WebServer server(80);
Preferences preferences;

String adminPassword;
String savedSsid;
String savedPassword;

// BLE のコールバックは BLE タスクから呼ばれる。フラッシュ書き込みや
// WiFi.scanNetworks() をそこで実行すると BLE スタックを止めてしまうので、
// コールバックはフラグと入力値を置くだけにして、実処理は loop() で行う。
// publishStatus() も同様で、loop() が書き換える String を BLE タスクから
// 読まないよう statusDirty 経由で loop() に委ねる。
//
// pending* は BLE タスクが書き、loop() は対応するフラグが立った後にだけ
// 読んで消費する。フラグは値を書き終えた後に立てるので、両者が同時に触ることはない。
volatile bool authRequested = false;
volatile bool scanRequested = false;
volatile bool connectRequested = false;
volatile bool disconnectRequested = false;
volatile bool forgetRequested = false;
volatile bool savePasswordRequested = false;
volatile bool statusDirty = false;
String pendingAuthPassword;
String pendingSsid;
String pendingPassword;
String pendingNewAdminPassword;

volatile bool authenticated = false;
volatile bool bleConnected = false;
volatile WifiState wifiState = WifiState::Idle;
unsigned long wifiConnectStartedAt = 0;
bool webServerStarted = false;

const char *wifiStateName() {
  switch (wifiState) {
    case WifiState::Scanning: return "scanning";
    case WifiState::Connecting: return "connecting";
    case WifiState::Connected: return "connected";
    case WifiState::Failed: return "failed";
    default: return "idle";
  }
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      escaped += '\\';
      escaped += c;
    } else if (c >= 0x20) {
      escaped += c;
    }
  }
  return escaped;
}

void publishStatus() {
  String json = "{";
  json += "\"auth\":";
  json += authenticated ? "true" : "false";
  json += ",\"wifi\":\"";
  json += wifiStateName();
  json += "\",\"savedSsid\":\"";
  json += jsonEscape(savedSsid);
  json += "\",\"ip\":\"";
  json += wifiState == WifiState::Connected ? WiFi.localIP().toString() : String("");
  json += "\",\"rssi\":";
  json += wifiState == WifiState::Connected ? String(WiFi.RSSI()) : String(0);
  json += "}";

  statusCharacteristic->setValue(json.c_str());
  statusCharacteristic->notify();
}

// ---------------------------------------------------------------- HTTP server

String contentTypeFor(const String &path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico")) return "image/x-icon";
  return "text/plain";
}

bool serveFile(String path) {
  if (path.indexOf("..") >= 0) return false;
  if (path.endsWith("/")) path += "index.html";
  if (!LittleFS.exists(path)) return false;

  File file = LittleFS.open(path, "r");
  server.streamFile(file, contentTypeFor(path));
  file.close();
  return true;
}

void handleApiStatus() {
  String json = "{";
  json += "\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"mac\":\"" + WiFi.macAddress() + "\"";
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"uptimeSeconds\":" + String(millis() / 1000);
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"bleConnected\":";
  json += bleConnected ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void startWebServer() {
  if (webServerStarted) return;

  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.onNotFound([]() {
    if (!serveFile(server.uri())) {
      server.send(404, "text/plain", "Not Found");
    }
  });
  server.begin();
  webServerStarted = true;

  Serial.printf("HTTP server started: http://%s/\n", WiFi.localIP().toString().c_str());
}

// ------------------------------------------------------------------ Wi-Fi

void beginWifiConnect() {
  if (savedSsid.length() == 0) {
    wifiState = WifiState::Failed;
    publishStatus();
    return;
  }

  WiFi.disconnect();
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  wifiState = WifiState::Connecting;
  wifiConnectStartedAt = millis();
  publishStatus();

  Serial.printf("Connecting to %s...\n", savedSsid.c_str());
}

void pollWifi() {
  switch (wifiState) {
    case WifiState::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        wifiState = WifiState::Connected;
        startWebServer();
        publishStatus();
      } else if (millis() - wifiConnectStartedAt >= WIFI_CONNECT_TIMEOUT) {
        WiFi.disconnect();
        wifiState = WifiState::Failed;
        publishStatus();
        Serial.println("Wi-Fi connection timed out");
      }
      break;

    case WifiState::Connected:
      if (WiFi.status() != WL_CONNECTED) {
        wifiState = WifiState::Connecting;
        wifiConnectStartedAt = millis();
        WiFi.reconnect();
        publishStatus();
        Serial.println("Wi-Fi lost, reconnecting");
      }
      break;

    default:
      break;
  }
}

void runScan() {
  WifiState previousState = wifiState;
  wifiState = WifiState::Scanning;
  publishStatus();

  int found = WiFi.scanNetworks();
  if (found < 0) found = 0;  // WIFI_SCAN_FAILED

  String json = "[";
  for (int i = 0; i < found && i < MAX_SCAN_RESULTS; i++) {
    String entry = i == 0 ? "" : ",";
    entry += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\"";
    entry += ",\"rssi\":" + String(WiFi.RSSI(i));
    entry += ",\"open\":";
    entry += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false";
    entry += "}";

    if (json.length() + entry.length() + 1 > SCAN_JSON_LIMIT) break;
    json += entry;
  }
  json += "]";
  WiFi.scanDelete();

  scanCharacteristic->setValue(json.c_str());
  wifiState = previousState;
  publishStatus();
}

// Idle に落とすことで pollWifi() の自動再接続を止める。認証情報は残すので
// control:connect で繋ぎ直せる。
void disconnectWifi() {
  WiFi.disconnect();
  wifiState = WifiState::Idle;
  publishStatus();
  Serial.println("Wi-Fi disconnected");
}

void forgetWifi() {
  preferences.remove("ssid");
  preferences.remove("pass");
  savedSsid = "";
  savedPassword = "";
  disconnectWifi();
  Serial.println("Saved Wi-Fi credentials cleared");
}

// -------------------------------------------------------------------- BLE

class AuthCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    pendingAuthPassword = characteristic->getValue();
    // クライアントが読み戻せないよう、照合後は値を残さない
    characteristic->setValue("");
    authRequested = true;
  }
};

class SsidCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    if (!authenticated) return;
    pendingSsid = characteristic->getValue();
    characteristic->setValue("");
  }
};

class PasswordCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    if (!authenticated) return;
    pendingPassword = characteristic->getValue();
    characteristic->setValue("");
  }
};

class ControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    if (!authenticated) return;
    String command = characteristic->getValue();

    if (command == "scan") {
      scanRequested = true;
    } else if (command == "connect") {
      connectRequested = true;
    } else if (command == "disconnect") {
      disconnectRequested = true;
    } else if (command == "forget") {
      forgetRequested = true;
    } else if (command.startsWith("setpw:")) {
      pendingNewAdminPassword = command.substring(6);
      savePasswordRequested = true;
    } else {
      // ここに来るのはたいてい書き込み済みファームウェアが setup.html より古いとき
      Serial.printf("Unknown control command: %s\n", command.c_str());
    }
    characteristic->setValue("");
  }
};

class ConnectionCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *bleServer) override {
    bleConnected = true;
    digitalWrite(connectionLedPin, HIGH);
  }

  void onDisconnect(BLEServer *bleServer) override {
    bleConnected = false;
    digitalWrite(connectionLedPin, LOW);
    authenticated = false;
    advertising->start();
  }
};

void setupBle() {
  BLEDevice::init("ESP32-WWW");
  BLEDevice::setMTU(517);

  BLEServer *bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ConnectionCallbacks());

  BLEService *service = bleServer->createService(BLEUUID(SERVICE_UUID), 32);

  BLECharacteristic *authCharacteristic = service->createCharacteristic(
      AUTH_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
  authCharacteristic->setCallbacks(new AuthCallbacks());

  BLECharacteristic *ssidCharacteristic = service->createCharacteristic(
      SSID_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
  ssidCharacteristic->setCallbacks(new SsidCallbacks());

  BLECharacteristic *passwordCharacteristic = service->createCharacteristic(
      PASSWORD_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
  passwordCharacteristic->setCallbacks(new PasswordCallbacks());

  BLECharacteristic *controlCharacteristic = service->createCharacteristic(
      CONTROL_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
  controlCharacteristic->setCallbacks(new ControlCallbacks());

  statusCharacteristic = service->createCharacteristic(
      STATUS_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  statusCharacteristic->addDescriptor(new BLE2902());

  scanCharacteristic = service->createCharacteristic(
      SCAN_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_READ);
  scanCharacteristic->setValue("[]");

  service->start();

  advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->start();
}

// ------------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);

  pinMode(connectionLedPin, OUTPUT);
  digitalWrite(connectionLedPin, LOW);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed: upload the data/ folder first");
  }

  preferences.begin(PREFERENCES_NAMESPACE, false);
  adminPassword = preferences.getString("adminpw", DEFAULT_ADMIN_PASSWORD);
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("pass", "");

  WiFi.mode(WIFI_STA);
  setupBle();
  publishStatus();

  if (savedSsid.length() > 0) {
    beginWifiConnect();
  }
}

void loop() {
  if (authRequested) {
    authRequested = false;
    authenticated = pendingAuthPassword.length() > 0 && pendingAuthPassword == adminPassword;
    statusDirty = true;
  }

  if (savePasswordRequested) {
    savePasswordRequested = false;
    if (pendingNewAdminPassword.length() > 0) {
      adminPassword = pendingNewAdminPassword;
      preferences.putString("adminpw", adminPassword);
      Serial.println("Admin password updated");
    }
  }

  if (connectRequested) {
    connectRequested = false;
    if (pendingSsid.length() > 0) {
      savedSsid = pendingSsid;
      savedPassword = pendingPassword;
      preferences.putString("ssid", savedSsid);
      preferences.putString("pass", savedPassword);
      // 使い終えた値を残すと、次の connect が保存済みの設定ではなく
      // これを拾ってしまう (forget 後の再接続で認証情報が甦る)
      pendingSsid = "";
      pendingPassword = "";
    }
    beginWifiConnect();
  }

  if (disconnectRequested) {
    disconnectRequested = false;
    disconnectWifi();
  }

  if (forgetRequested) {
    forgetRequested = false;
    forgetWifi();
  }

  if (scanRequested) {
    scanRequested = false;
    runScan();
  }

  pollWifi();

  if (statusDirty) {
    statusDirty = false;
    publishStatus();
  }

  if (webServerStarted) {
    server.handleClient();
  }
}
