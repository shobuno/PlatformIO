#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoOTA.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Adafruit_ADS1X15.h>  // 追加
#include "esp_task_wdt.h"
#include "esp_sleep.h"

WebSocketsClient webSocket;
Adafruit_ADS1115 ads;  // 追加

const char* ws_host = "192.168.0.2";
const uint16_t ws_port = 3000;
const char* ws_path = "/ws";

float temp_1 = 0.0;
float temp_2 = 0.0;
String serNo_1 = "";
String serNo_2 = "";
float ecAnalog = 0.0;

const char* ssid = "F4239C66A319-5G";
const char* password = "er7hmxby57akes";

IPAddress local_IP(192, 168, 0, 20);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

const char* serverUrl = "http://192.168.0.2:3000/api/sensor";
const char* waterLevelUrl = "http://192.168.0.2:3000/api/water-level";

#define ONE_WIRE_BUS 13
#define ECPower 33
#define WATER_LEVEL_PIN_1 5
#define WATER_LEVEL_PIN_2 16
#define WATER_LEVEL_PIN_3 17

unsigned long lastSendTime = 0;
const unsigned long sendInterval = 60000;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

void handleRelayCommand(int relay, bool value) {
  Serial.printf("[Relay] Command received: relay %d → %s\n", relay, value ? "ON" : "OFF");
}

void otaTask(void* parameter) {
  for (;;) {
    ArduinoOTA.handle();
    vTaskDelay(10 / portTICK_PERIOD_MS);
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
    if (millis() - wifiStart > 10000) {
      ESP.restart();
    }
  }

  pinMode(ECPower, OUTPUT);
  digitalWrite(ECPower, LOW);
  delay(100);

  sensors.begin();
  Wire.begin(21, 22);         // I2Cピンを指定して開始（SDA=21, SCL=22）
  ads.begin();                // ADS1115の初期化

  esp_task_wdt_init(8, true);
  esp_task_wdt_add(NULL);

  randomSeed(esp_random());
}

void sendSensorData();
void sendWaterLevel();

void loop() {
  esp_task_wdt_reset();

  sendSensorData();
  sendWaterLevel();

  Serial.flush();

  esp_sleep_enable_timer_wakeup(sendInterval * 1000);
  esp_deep_sleep_start();
}

void getMyTemperature() {
  DeviceAddress deviceAddress;
  sensors.requestTemperatures();
  for (int i = 0; i < sensors.getDeviceCount(); i++) {
    if (!sensors.getAddress(deviceAddress, i)) continue;
    float temp = sensors.getTempC(deviceAddress);
    String serNo = "";
    for (int j = 0; j < 8; j++) {
      if (deviceAddress[j] < 0x10) serNo += "0";
      serNo += String(deviceAddress[j], HEX);
    }
    if (i == 0) {
      serNo_1 = serNo;
      temp_1 = temp;
    } else if (i == 1) {
      serNo_2 = serNo;
      temp_2 = temp;
    }
    delay(30);
  }
  webSocket.loop();
}

void getECvalue(){
  digitalWrite(ECPower, HIGH);
  delay(2);
  ecAnalog = ads.readADC_SingleEnded(0); // ADS1115 A0を読み取る
  digitalWrite(ECPower, LOW);
}

void sendSensorData() {
  getMyTemperature();
  getECvalue();

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverUrl);
    http.addHeader("Content-Type", "application/json");

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
  int level1 = digitalRead(WATER_LEVEL_PIN_1);
  int level2 = digitalRead(WATER_LEVEL_PIN_2);
  int level3 = digitalRead(WATER_LEVEL_PIN_3);
  Serial.println("Water Level Pins: " + String(level1) + ", " + String(level2) + ", " + String(level3));

  int level = 3 - (level1 + level2 + level3);

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(waterLevelUrl);
    http.addHeader("Content-Type", "application/json");

    String json = "{";
    json += "\"serial_number\":\"WL-0001\",";
    json += "\"water_level\":" + String(level);
    json += "}";

    int res = http.POST(json);
    http.end();
  }
}
