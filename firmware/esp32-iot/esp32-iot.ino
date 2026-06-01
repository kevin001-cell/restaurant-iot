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

// ========== LED 状态 ==========
bool   ledState = false;
int    ledBrightness = 0;    // 0-100
const int LED_PIN = 5;       // GPIO5 PWM 输出
volatile bool statusReportPending = false;

// ========== 上报计时 ==========
unsigned long lastReportTime = 0;
const unsigned long REPORT_INTERVAL = 60000;  // 60秒

// ========== MQTT Topic 拼接 ==========
const String TOPIC_POST  = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post";
const String TOPIC_SET   = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/set";
const String TOPIC_REPLY = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post/reply";

void reconnectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("MQTT connecting...");

    // OneNET 认证: username=product_id, password=device_key
    String clientId = DEVICE_NAME;
    String username = PRODUCT_ID;
    String password = DEVICE_KEY;

    if (mqtt.connect(clientId.c_str(), username.c_str(), password.c_str())) {
      Serial.println("connected");
      // 订阅下行控制指令
      mqtt.subscribe(TOPIC_SET.c_str());
      Serial.print("Subscribed: ");
      Serial.println(TOPIC_SET);
      // 订阅上报回执 (可选)
      mqtt.subscribe(TOPIC_REPLY.c_str());
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 5s");
      delay(5000);
    }
  }
}

// ========== MQTT 消息回调 (接收云端下发的 LED 控制指令) ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.concat((char*)payload, length);
  Serial.print("Received: ");
  Serial.println(msg);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) return;

  JsonObject params = doc["params"].as<JsonObject>();
  if (params.containsKey("led_switch")) {
    ledState = params["led_switch"]["value"].as<int>() == 1;
  }
  if (params.containsKey("led_brightness")) {
    ledBrightness = params["led_brightness"]["value"].as<int>();
  }

  int pwmValue = ledState ? map(ledBrightness, 0, 100, 0, 255) : 0;
  analogWrite(LED_PIN, pwmValue);
  Serial.printf("LED: %s, brightness: %d%%\n", ledState ? "ON" : "OFF", ledBrightness);

  // 延迟上报: mqtt.publish 不能在回调中直接调用
  statusReportPending = true;
}

// ========== 上报温湿度数据 ==========
void reportTelemetry() {
  sht30.readBoth();
  float temp = sht30.readTemperature();
  float hum  = sht30.readHumidity();

  if (isnan(temp) || isnan(hum)) {
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
  params.createNestedObject("led_brightness")["value"]  = ledBrightness;

  String payload;
  serializeJson(doc, payload);

  if (mqtt.publish(TOPIC_POST.c_str(), payload.c_str())) {
    Serial.printf("Reported: %.1f°C, %.0f%%\n", temp, hum);
  } else {
    Serial.println("Publish failed");
  }
}

// ========== 上报 LED 状态回执 ==========
void reportStatus() {
  StaticJsonDocument<256> doc;
  char id[16];
  snprintf(id, 16, "%lu", millis());
  doc["id"] = id;
  JsonObject params = doc.createNestedObject("params");
  params.createNestedObject("led_switch")["value"]      = ledState ? 1 : 0;
  params.createNestedObject("led_brightness")["value"]  = ledBrightness;

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_POST.c_str(), payload.c_str());
}

// ========== Arduino Setup ==========
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());
  delay(1000);
  Serial.println("\n=== ESP32S3 IoT Starting ===");

  // 初始化 LED
  pinMode(LED_PIN, OUTPUT);
  analogWrite(LED_PIN, 0);

  // 初始化 I2C (SHT30)
  Wire.begin(21, 22);  // SDA=GPIO21, SCL=GPIO22
  int shtRetries = 0;
  while (!sht30.begin(0x44) && shtRetries < 3) {
    Serial.println("SHT30 init retry...");
    delay(1000);
    shtRetries++;
  }
  if (!sht30.begin(0x44)) {
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
}

// ========== Arduino Loop ==========
void loop() {
  if (!mqtt.connected()) {
    reconnectMQTT();
  }
  mqtt.loop();

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
