// firmware/esp32-iot/esp32-iot.ino
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
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
const String TOPIC_UP   = "$thing/up/property/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME);
const String TOPIC_DOWN = "$thing/down/property/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME);

// ========== HMAC-SHA1 Base64 密码生成 (腾讯云 IoT 认证) ==========
// username = PRODUCT_ID DEVICE_NAME;12010126;{connid};{expiry}
// password = base64(hmac_sha1(device_secret, username))
String generateMQTTPassword(const String& username) {
  // HMAC-SHA1
  uint8_t hmac[20];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)DEVICE_SECRET, strlen(DEVICE_SECRET));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)username.c_str(), username.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);

  // Base64 encode
  const char* b64table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String password = "";
  for (int i = 0; i < 20; i += 3) {
    uint32_t v = ((uint32_t)hmac[i] << 16) | ((i+1 < 20 ? (uint32_t)hmac[i+1] : 0) << 8) | (i+2 < 20 ? (uint32_t)hmac[i+2] : 0);
    password += b64table[(v >> 18) & 0x3F];
    password += b64table[(v >> 12) & 0x3F];
    password += b64table[(v >> 6) & 0x3F];
    password += b64table[v & 0x3F];
  }
  // 处理 padding
  int rem = 20 % 3;
  if (rem > 0) {
    password[password.length() - 1] = '=';
    if (rem == 1) password[password.length() - 2] = '=';
  }

  return password;
}

void reconnectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("MQTT connecting...");

    String connid = String(random(10000, 99999));
    String username = String(PRODUCT_ID) + DEVICE_NAME + ";12010126;" + connid + ";0";
    String password = generateMQTTPassword(username);
    String clientId = String(PRODUCT_ID) + DEVICE_NAME;

    if (mqtt.connect(clientId.c_str(), username.c_str(), password.c_str())) {
      Serial.println("connected");
      mqtt.subscribe(TOPIC_DOWN.c_str());
      Serial.print("Subscribed: ");
      Serial.println(TOPIC_DOWN);
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

  if (doc["method"] == "control") {
    JsonObject params = doc["params"].as<JsonObject>();

    if (params.containsKey("led_switch")) {
      ledState = params["led_switch"].as<int>() == 1;
    }
    if (params.containsKey("led_brightness")) {
      ledBrightness = params["led_brightness"].as<int>();
    }

    int pwmValue = ledState ? map(ledBrightness, 0, 100, 0, 255) : 0;
    analogWrite(LED_PIN, pwmValue);
    Serial.printf("LED: %s, brightness: %d%%\n", ledState ? "ON" : "OFF", ledBrightness);

    // 延迟上报: mqtt.publish 不能在回调中直接调用
    statusReportPending = true;
  }
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

  StaticJsonDocument<200> doc;
  doc["method"] = "report";
  doc["clientToken"] = String(random(100000, 999999));
  JsonObject params = doc.createNestedObject("params");
  params["temperature"] = round(temp * 10) / 10.0;  // 保留1位小数
  params["humidity"]    = round(hum);

  String payload;
  serializeJson(doc, payload);

  if (mqtt.publish(TOPIC_UP.c_str(), payload.c_str())) {
    Serial.printf("Reported: %.1f°C, %.0f%%\n", temp, hum);
  } else {
    Serial.println("Publish failed");
  }
}

// ========== 上报 LED 状态回执 ==========
void reportStatus() {
  StaticJsonDocument<200> doc;
  doc["method"] = "report";
  doc["clientToken"] = String(random(100000, 999999));
  JsonObject params = doc.createNestedObject("params");
  params["led_switch"] = ledState ? 1 : 0;
  params["led_brightness"] = ledBrightness;

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_UP.c_str(), payload.c_str());
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
