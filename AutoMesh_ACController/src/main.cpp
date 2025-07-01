// ESP32 Relay Control with WebSocket, OTA, and NeoPixel Breathe Animation

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>  // 追加

#define WDT_TIMEOUT 10  // 秒

// --- プロトタイプ宣言 ---
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
void reconnectWebSocket();
bool isDeviceRegistered();
void breatheTask(void* parameter);
void loadRelayStatesFromEEPROM();  // ←追加
void saveRelayStatesToEEPROM();    // ←追加

// --- Wi-Fi設定 ---
const char* ssid = "F4239C66A319-5G";
const char* password = "er7hmxby57akes";

// --- 固定IP ---
IPAddress local_IP(192, 168, 0, 17);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// --- EEPROM設定 ---
#define EEPROM_SIZE 64
#define EEPROM_REGISTERED_ADDR 0
#define EEPROM_RELAY_STATE_ADDR 1  // 追加: リレー状態保存用

// --- WebSocket設定 ---
WebSocketsClient webSocket;
const char* ws_host = "192.168.0.2";
const uint16_t ws_port = 3000;
String currentWsPath = "/automesh-entry";
unsigned long lastReconnectAttempt = 0;

// --- リレー定義 ---
const int relayPins[] = {26, 27};
const int NUM_RELAYS = sizeof(relayPins) / sizeof(relayPins[0]);

// --- NeoPixel（PL9823）設定 ---
#define LED_PIN 17
#define NUM_LEDS 2
Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

bool relayStates[NUM_LEDS] = {false, false};
TaskHandle_t breatheTasks[NUM_LEDS];

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  // --- WDT初期化 ---
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("❌ STA config failed");
  }
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("✅ Connected! IP: " + WiFi.localIP().toString());

  loadRelayStatesFromEEPROM(); // リレー状態をEEPROMから復元

  pixels.begin();
  pixels.clear();
  pixels.show();

  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], relayStates[i] ? HIGH : LOW); // EEPROM復元値で初期化
    xTaskCreatePinnedToCore(breatheTask, ("breathe_" + String(i)).c_str(), 2048, (void*)(uintptr_t)i, 1, &breatheTasks[i], 1);
  }

  currentWsPath = isDeviceRegistered() ? "/automesh-command" : "/automesh-entry";
  reconnectWebSocket();

  ArduinoOTA.setHostname("esp32-relay");
  ArduinoOTA.begin();
  xTaskCreatePinnedToCore([](void*) {
    for (;;) {
      ArduinoOTA.handle();
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }
  }, "OTA_Task", 4096, NULL, 1, NULL, 0);
}

void loop() {
  webSocket.loop();
  ArduinoOTA.handle();
  esp_task_wdt_reset(); // WDTリセット
}

// --- EEPROM関連 ---
bool isDeviceRegistered() {
  return EEPROM.read(EEPROM_REGISTERED_ADDR) == 1;
}
void markDeviceAsRegistered() {
  EEPROM.write(EEPROM_REGISTERED_ADDR, 1);
  EEPROM.commit();
  Serial.println("✅ markDeviceAsRegistered");
}
void clearDeviceRegistration() {
  EEPROM.write(EEPROM_REGISTERED_ADDR, 0);
  EEPROM.commit();
  Serial.println("🧹 clearDeviceRegistration");
}

void saveRelayStatesToEEPROM() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    EEPROM.write(EEPROM_RELAY_STATE_ADDR + i, relayStates[i] ? 1 : 0);
  }
  EEPROM.commit();
}

void loadRelayStatesFromEEPROM() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    relayStates[i] = EEPROM.read(EEPROM_RELAY_STATE_ADDR + i) == 1;
  }
}

// --- リレー状態送信 ---
void sendRelayState(int index, bool state) {
  String json = "{\"type\":\"relay-state\",\"relay_index\":" + String(index) +
                ",\"state\":" + (state ? "true" : "false") + "}";
  Serial.println("📤 Relay状態送信: " + json);
  webSocket.sendTXT(json);
}

// --- Breatheアニメーション処理 ---
void breatheTask(void* parameter) {
  int index = (int)(uintptr_t)parameter;
  const int max_brightness = 30;
  const int delay_ms = 60;

  while (true) {
    if (!relayStates[index]) {
      for (int b = 0; b <= max_brightness && !relayStates[index]; b++) {
        pixels.setPixelColor(index, pixels.Color(0, 0, b));
        pixels.show();
        delay(delay_ms);
      }
      for (int b = max_brightness; b >= 0 && !relayStates[index]; b--) {
        pixels.setPixelColor(index, pixels.Color(0, 0, b));
        pixels.show();
        delay(delay_ms);
      }
      delay(delay_ms * 100); // 少し待機
    } else {
      pixels.setPixelColor(index, pixels.Color(80, 80, 80));
      pixels.show();
      delay(100);
    }
  }
}

// --- リレー制御 ---
void handleRelay(int index, bool state) {
  if (index < 0 || index >= NUM_RELAYS) return;
  digitalWrite(relayPins[index], state ? HIGH : LOW);
  relayStates[index] = state;
  saveRelayStatesToEEPROM(); // 状態を保存
  if (state) {
    pixels.setPixelColor(index, pixels.Color(80, 80, 80));
    pixels.show();
  }
  String log = "Relay " + String(index) + (state ? " ON" : " OFF");
  webSocket.sendTXT("{\"type\":\"log\",\"message\":\"" + log + "\"}");
  sendRelayState(index, state);
}

// --- 点滅処理 ---
void handleBlink(int index) {
  for (int i = 0; i < 10; i++) {
    pixels.setPixelColor(index, pixels.Color(255, 255, 0)); // 黄色
    pixels.show(); delay(300);
    pixels.setPixelColor(index, pixels.Color(0, 0, 0));     // 消灯
    pixels.show(); delay(300);
  }
  if (!relayStates[index]) {
    pixels.setPixelColor(index, pixels.Color(0, 0, 10));
    pixels.show();
  }
}

// --- WebSocket再接続 ---
void reconnectWebSocket() {
  Serial.println("🔁 reconnectWebSocket → " + currentWsPath);
  delay(500);
  webSocket.disconnect();
  webSocket.onEvent(webSocketEvent);
  webSocket.begin(ws_host, ws_port, currentWsPath.c_str());
  webSocket.setReconnectInterval(5000);
}

// --- WebSocketイベント処理 ---
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("✅ WebSocket connected");
      webSocket.setReconnectInterval(5000);
      if (isDeviceRegistered()) {
        webSocket.sendTXT("{\"type\":\"command-entry\",\"serial_number\":\"esp32-relay-01\"}");
      } else {
        webSocket.sendTXT("{\"type\":\"entry\",\"serial_number\":\"esp32-relay-01\"}");
      }
      break;
    case WStype_DISCONNECTED: {
      {
        unsigned long now = millis();
        if (now - lastReconnectAttempt < 10000) return;
        lastReconnectAttempt = now;
        currentWsPath = isDeviceRegistered() ? "/automesh-command" : "/automesh-entry";
        reconnectWebSocket();
      }
      break;
    }
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length)) return;
      String type = doc["type"];
      if (type == "registered") {
        markDeviceAsRegistered();
        webSocket.disconnect(); delay(500);
        currentWsPath = "/automesh-command";
        reconnectWebSocket();
      } else if (type == "relay-toggle") {
        int index = doc["relay_index"];
        bool value = doc["on"];
        handleRelay(index, value);
      } else if (type == "led-blink") {
        int index = doc["relay_index"];
        handleBlink(index);
      } else if (type == "unregistered") {
        clearDeviceRegistration();
        webSocket.disconnect(); delay(500);
        currentWsPath = "/automesh-entry";
        reconnectWebSocket();
      }
      break;
    }
    default:
      break;
  }
}
