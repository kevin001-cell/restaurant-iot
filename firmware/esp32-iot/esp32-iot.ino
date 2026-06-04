// firmware/esp32-iot/esp32-iot.ino
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <ArduinoJson.h>
#include "config.h"

// ========== 全局对象 ==========
Adafruit_SHT31 sht30 = Adafruit_SHT31();
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// 公共 MQTT Broker (Web 控制台中继)
WiFiClient pubWifiClient;
PubSubClient pubMqtt(pubWifiClient);

// ========== LED 状态 ==========
bool   ledState = false;
int    ledBrightness = 0;    // 0-100
const int LED_PIN = 5;       // GPIO5 PWM 输出
volatile bool statusReportPending = false;

// ========== SHT30 状态 ==========
bool shtAvailable = false;

// ========== 上报计时 ==========
unsigned long lastReportTime = 0;
const unsigned long REPORT_INTERVAL = 60000;  // 60秒

// ========== MQTT Topic 拼接 ==========
const String TOPIC_POST  = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post";
const String TOPIC_SET   = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/set";

void reconnectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("MQTT connecting...");

    // OneNET 认证: username=product_id, password=device_key
    String clientId = DEVICE_NAME;
    String username = PRODUCT_ID;
    String password = MQTT_TOKEN;

    if (mqtt.connect(clientId.c_str(), username.c_str(), password.c_str())) {
      Serial.println("connected");
      // 订阅下行控制指令
      mqtt.subscribe(TOPIC_SET.c_str());
      Serial.print("Subscribed: ");
      Serial.println(TOPIC_SET);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 5s");
      delay(5000);
    }
  }
}

// ========== 公共 MQTT Broker 重连 ==========
void reconnectPubMQTT() {
  while (!pubMqtt.connected()) {
    Serial.print("Pub MQTT connecting...");
    // 公共 broker 无需认证
    String clientId = String(DEVICE_NAME) + "_" + String(random(1000, 9999));
    if (pubMqtt.connect(clientId.c_str())) {
      Serial.println("connected");
      pubMqtt.subscribe(PUB_TOPIC_CONTROL);
      Serial.print("Subscribed: ");
      Serial.println(PUB_TOPIC_CONTROL);
    } else {
      Serial.print("failed, rc=");
      Serial.print(pubMqtt.state());
      Serial.println(" retrying in 10s");
      delay(10000);
    }
  }
}

// ========== 公共 MQTT 控制回调 ==========
void pubMqttCallback(char* topic, byte* payload, unsigned int length) {
  // 只处理控制指令
  if (strstr(topic, PUB_TOPIC_CONTROL) == NULL) return;

  String msg;
  msg.concat((char*)payload, length);
  Serial.print("Pub control received: ");
  Serial.println(msg);

  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) return;

  if (doc.containsKey("led")) {
    ledState = doc["led"].as<int>() == 1;
  }
  if (doc.containsKey("bright")) {
    ledBrightness = doc["bright"].as<int>();
  }

  int pwmValue = ledState ? map(ledBrightness, 0, 100, 0, 255) : 0;
  analogWrite(LED_PIN, pwmValue);
  Serial.printf("LED: %s, brightness: %d%%\n", ledState ? "ON" : "OFF", ledBrightness);

  // 延迟上报状态
  statusReportPending = true;
}

// ========== MQTT 消息回调 (接收云端下发的 LED 控制指令) ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.concat((char*)payload, length);
  Serial.print("Received: ");
  Serial.println(msg);

  // 只处理下行控制指令，忽略回执等其他消息
  if (strstr(topic, "/thing/property/set") == NULL) return;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) return;

  JsonObject params = doc["params"].as<JsonObject>();
  if (params.containsKey("led_switch")) {
    ledState = params["led_switch"]["value"].as<int>() == 1;
  }
  if (params.containsKey("led_bright")) {
    ledBrightness = params["led_bright"]["value"].as<int>();
  }

  int pwmValue = ledState ? map(ledBrightness, 0, 100, 0, 255) : 0;
  analogWrite(LED_PIN, pwmValue);
  Serial.printf("LED: %s, brightness: %d%%\n", ledState ? "ON" : "OFF", ledBrightness);

  // 延迟上报: mqtt.publish 不能在回调中直接调用
  statusReportPending = true;
}

// ========== 上报温湿度数据 ==========
void reportTelemetry() {
  if (!shtAvailable) {
    Serial.println("SHT30 not available, skip read");
    return;
  }

  float temp, hum;
  if (!sht30.readBoth(&temp, &hum)) {
    Serial.println("SHT30 read error");
    return;
  }

  StaticJsonDocument<256> doc;
  char id[16];
  snprintf(id, 16, "%lu", millis());
  doc["id"] = id;
  JsonObject params = doc.createNestedObject("params");
  params.createNestedObject("temperature")["value"]     = round(temp * 10) / 10.0;  // 保留1位小数
  params.createNestedObject("humidity")["value"]        = round(hum);
  params.createNestedObject("led_switch")["value"]      = ledState ? 1 : 0;
  params.createNestedObject("led_bright")["value"]  = ledBrightness;

  String payload;
  serializeJson(doc, payload);

  if (mqtt.publish(TOPIC_POST.c_str(), payload.c_str())) {
    Serial.printf("Reported: %.1f°C, %.0f%%\n", temp, hum);
  } else {
    Serial.println("Publish failed");
  }

  // 同时向公共 broker 推送简化格式 (Web 控制台实时数据)
  StaticJsonDocument<128> pubDoc;
  pubDoc["temp"]   = round(temp * 10) / 10.0;
  pubDoc["hum"]    = round(hum);
  pubDoc["led"]    = ledState ? 1 : 0;
  pubDoc["bright"] = ledBrightness;
  String pubPayload;
  serializeJson(pubDoc, pubPayload);
  pubMqtt.publish(PUB_TOPIC_TELEMETRY, pubPayload.c_str());
}

// ========== 上报 LED 状态回执 ==========
void reportStatus() {
  StaticJsonDocument<256> doc;
  char id[16];
  snprintf(id, 16, "%lu", millis());
  doc["id"] = id;
  JsonObject params = doc.createNestedObject("params");
  params.createNestedObject("led_switch")["value"]      = ledState ? 1 : 0;
  params.createNestedObject("led_bright")["value"]  = ledBrightness;

  String payload;
  serializeJson(doc, payload);
  if (mqtt.publish(TOPIC_POST.c_str(), payload.c_str())) {
    Serial.println("LED status reported");
  } else {
    Serial.println("Status publish failed");
  }
}

// ========== Arduino Setup ==========
void setup() {
  Serial.begin(115200);
  // 等待 USB CDC 串口就绪
  int waitSerial = 0;
  while (!Serial && waitSerial < 50) { delay(100); waitSerial++; }
  Serial.println("\n=== ESP32S3 IoT Starting ===");

  // 初始化 LED
  pinMode(LED_PIN, OUTPUT);
  analogWrite(LED_PIN, 0);

  // 初始化 I2C (SHT30)
  Wire.begin(15, 16);  // SDA=GPIO15, SCL=GPIO16
  bool shtOk = false;
  int shtRetries = 0;
  while (shtRetries < 3) {
    if (sht30.begin(0x44)) { shtOk = true; break; }
    Serial.println("SHT30 init retry...");
    delay(1000);
    shtRetries++;
  }
  shtAvailable = shtOk;
  if (!shtOk) {
    Serial.println("SHT30 init failed! Will retry on each report.");
  } else {
    Serial.println("SHT30 OK");
  }

  // 连接 WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  int wifiRetries = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetries < 40) {
    Serial.print(".");
    delay(500);
    wifiRetries++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed, restarting...");
    ESP.restart();
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  // 初始化 MQTT
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  reconnectMQTT();

  // 初始化公共 MQTT Broker (Web 控制台中继)
  pubMqtt.setServer(PUB_BROKER, PUB_PORT);
  pubMqtt.setCallback(pubMqttCallback);
  reconnectPubMQTT();
}

// ========== Arduino Loop ==========
void loop() {
  if (!mqtt.connected()) {
    reconnectMQTT();
  }
  mqtt.loop();

  if (!pubMqtt.connected()) {
    reconnectPubMQTT();
  }
  pubMqtt.loop();

  // 延迟状态上报 (从回调中推迟到 loop)
  if (statusReportPending) {
    statusReportPending = false;
    reportStatus();
  }

  unsigned long now = millis();
  if (now - lastReportTime >= REPORT_INTERVAL) {
    lastReportTime = now;
    reportTelemetry();
  }
}
