#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoOTA.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>  // ← 必ず必要！
#include "esp_task_wdt.h"  // Watchdog追加
#include "esp_sleep.h"  // 追加


WebSocketsClient webSocket;

const char* ws_host = "192.168.0.2";  // ←適宜設定
const uint16_t ws_port = 3000;
const char* ws_path = "/ws";

float temp_1 = 0.0;
float temp_2 = 0.0;
String serNo_1 = "";
String serNo_2 = "";
float ecAnalog = 0.0;


// Wi-Fi設定
const char* ssid = "F4239C66A319-5G";
const char* password = "er7hmxby57akes";

// 固定IP設定（自分のネットワーク環境に合わせて変更）
IPAddress local_IP(192, 168, 0, 20);     // ← 固定したいIP
IPAddress gateway(192, 168, 0, 1);        // ← 通常はルーターのIP
IPAddress subnet(255, 255, 255, 0);       // ← 一般的な設定
IPAddress primaryDNS(8, 8, 8, 8);         // ← 任意（Google DNSなど）
IPAddress secondaryDNS(8, 8, 4, 4);

// サーバーエンドポイント
const char* serverUrl = "http://192.168.0.2:3000/api/sensor";
const char* waterLevelUrl = "http://192.168.0.2:3000/api/water-level";

// 温度センサーピン
#define ONE_WIRE_BUS 13
int ECPin1 = 34;                // INPUT EC1
int ECGround = 25;              // OUTPUT
int ECPower = 33;               // OUTPUT

// 水位センサーのピン設定（仮実装）
#define WATER_LEVEL_PIN_1 5
#define WATER_LEVEL_PIN_2 16
#define WATER_LEVEL_PIN_3 17

// タイマー
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 60000; // 1分

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

void handleRelayCommand(int relay, bool value) {
  Serial.printf("[Relay] Command received: relay %d → %s\n", relay, value ? "ON" : "OFF");
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
      webSocket.sendTXT("{\"type\":\"register\",\"device\":\"hydrosense-esp32\"}");
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
  Serial.begin(115200);
  Serial.println("setup start");
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }

  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (millis() - wifiStart > 10000) { // 10秒で諦めてリセット
      ESP.restart();
    }
  }

  pinMode(ECPin1, INPUT);
  pinMode(ECPower, OUTPUT);
  pinMode(ECGround, OUTPUT);
  digitalWrite(ECGround, LOW);
  delay(100);

  sensors.begin();

  esp_task_wdt_init(8, true);
  esp_task_wdt_add(NULL);

  randomSeed(esp_random());
}

void sendSensorData();     // ← 追加
void sendWaterLevel();     // ← 追加

void loop() {
  esp_task_wdt_reset();

  sendSensorData(); // センサーデータを送信
  sendWaterLevel(); // 水位データを送信

  Serial.flush();

  esp_sleep_enable_timer_wakeup(sendInterval * 1000);
  esp_deep_sleep_start();
}

// 温度情報の取得
void getMyTemperature() {
  DeviceAddress deviceAddress;

  sensors.requestTemperatures();  // ★温度変換のリクエスト（重要）

  for (int i = 0; i < sensors.getDeviceCount(); i++) {

    if (!sensors.getAddress(deviceAddress, i)) {
      continue;
    }

    float temp = sensors.getTempC(deviceAddress);

    String serNo = "";
    for (int j = 0; j < 8; j++) {
      if (deviceAddress[j] < 0x10) serNo += "0";  // ゼロパディング
      serNo += String(deviceAddress[j], HEX);
    }
    if (i == 0) {
      serNo_1 = serNo;
      temp_1 = temp;
    } else if (i == 1) {
      serNo_2 = serNo;
      temp_2 = temp;
    }
    delay(30); // 30ミリ秒の送信間隔
  }

  webSocket.loop();
}


void getECvalue(){
  // EC測定 Start ********************************
  digitalWrite(ECPower, HIGH);
  ecAnalog = analogRead(ECPin1);
  delay(2);                     // esp32の計算速度が速いので2ミリ秒待機
  ecAnalog = analogRead(ECPin1);    // 静電容量の影響を抑えて再度計測
  digitalWrite(ECPower, LOW);
  // EC測定 End ********************************
}

void sendSensorData() {

  getMyTemperature();
  // 温度センサーの値を取得

  getECvalue();
  // ECセンサーの値を取得

  //Serial.printf("Temp_1: %.2f, Temp_2: %.2f, EC: %d\n", temp_1, temp_2, ecAnalog);

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverUrl);
    http.addHeader("Content-Type", "application/json");

    //serNo_2 ="28bd6149f6ce3cd5";

    String json = "{";
    json += "\"sensors\":[";
    json += "{\"serial\":\"" + serNo_1 + "\",\"value\":" + String(temp_1) + "},";
    json += "{\"serial\":\"" + serNo_2 + "\",\"value\":" + String(temp_2) + "}";
    json += "],";
    json += "\"ecAnalogValue\":" + String(ecAnalog);
    json += "}";

    Serial.println("Senddata:" + json);
    
    int res = http.POST(json);
    String response = http.getString();
    Serial.printf("POST /sensor result: %d\n", res);
    Serial.println("Response body: " + response);
    http.end();
  }
}

void sendWaterLevel() {
  int level1 = digitalRead(WATER_LEVEL_PIN_1); // 0～3の値なら analogRead に変更する
  int level2 = digitalRead(WATER_LEVEL_PIN_2); // 0～3の値なら analogRead に変更する
  int level3 = digitalRead(WATER_LEVEL_PIN_3); // 0～3の値なら analogRead に変更する
  Serial.println("Water Level Pins: " + String(level1) + ", " + String(level2) + ", " + String(level3));
  // 水位センサーの値を取得

  int level = 3;
  level = level - (level1 + level2 + level3); // 水位センサーの値を反転

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(waterLevelUrl);
    http.addHeader("Content-Type", "application/json");

    String json = "{";
    json += "\"serial_number\":\"WL-0001\",";
    json += "\"water_level\":" + String(level);
    json += "}";

    int res = http.POST(json);
    //Serial.printf("POST /water-level result: %d\n", res);
    http.end();
  }
 
}
