#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
//#include "esp_task_wdt.h"  // ヘッダを追加

const char* ssid = "F4239C66A319-5G";
const char* password = "er7hmxby57akes";

// 固定IP設定（自分のネットワーク環境に合わせて変更）
IPAddress local_IP(192, 168, 0, 17);     // ← 固定したいIP
IPAddress gateway(192, 168, 0, 1);        // ← 通常はルーターのIP
IPAddress subnet(255, 255, 255, 0);       // ← 一般的な設定
IPAddress primaryDNS(8, 8, 8, 8);         // ← 任意（Google DNSなど）
IPAddress secondaryDNS(8, 8, 4, 4);

WebSocketsClient webSocket;
const char* ws_host = "192.168.0.2";  // ← サーバーのIPアドレス
const uint16_t ws_port = 3000;
const char* ws_path = "/";

const int RELAY1_PIN = 26;
const int RELAY2_PIN = 27;

void handleRelayCommand(int relay, bool value) {
  if (relay == 1) digitalWrite(RELAY1_PIN, value ? HIGH : LOW);
  if (relay == 2) digitalWrite(RELAY2_PIN, value ? HIGH : LOW);

  String log = "Relay " + String(relay) + (value ? " ON" : " OFF");
  webSocket.sendTXT("{\"type\":\"log\",\"message\":\"" + log + "\"}");
}

void otaTask(void* parameter) {
  for (;;) {
    ArduinoOTA.handle();
    vTaskDelay(10 / portTICK_PERIOD_MS);  // 10msごとにチェック
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("WebSocket connected");
      webSocket.sendTXT("{\"type\":\"register\",\"device\":\"AutoMesh-esp32\"}");
      break;

    case WStype_TEXT:
      {
        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (!error && doc["type"] == "relay") {
          handleRelayCommand(doc["relay"], doc["value"]);
        }
      }
      break;
  }
}

void setupOTA() {
  ArduinoOTA.setHostname("esp32-relay");
  ArduinoOTA.begin();
}

void setup() {
  //esp_task_wdt_init(10, true);  // 10秒タイムアウト, panicあり
  //esp_task_wdt_add(NULL);       // 現在のタスク（loop）を監視対象に

  Serial.begin(115200);
  // IP設定（WiFi.beginより前に呼ぶ！）
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }
  WiFi.begin(ssid, password);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  webSocket.begin(ws_host, ws_port, ws_path);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  setupOTA();  // ArduinoOTA.begin()を含む

  // OTA用タスクをコア0で起動
  xTaskCreatePinnedToCore(
    otaTask,
    "OTA_Task",
    4096,
    NULL,
    1,
    NULL,
    0
  );
}

void loop() {
  webSocket.loop();
  ArduinoOTA.handle();
  //esp_task_wdt_reset();  // Watchdogキック（リセット）
}
