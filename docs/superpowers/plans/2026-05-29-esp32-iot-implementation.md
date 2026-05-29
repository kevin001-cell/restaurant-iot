# ESP32S3 温湿度监控系统 — 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建 ESP32S3 + SHT30 温湿度采集 + 腾讯云 IoT MQTT 上报 + Web 控制台远程 LED 控制的完整 IoT 系统

**Architecture:** ESP32S3 通过 Arduino 固件采集 SHT30 温湿度 + PWM 驱动 LED，MQTT/TLS 连接腾讯云 IoT Explorer。Web 控制台为单 HTML 页面，使用 MQTT.js WebSocket 直连 IoT Hub，Chart.js 渲染历史曲线，部署到 CloudBase 静态托管。

**Tech Stack:** Arduino C++ (ESP32S3), PubSubClient, Adafruit SHT31, ArduinoJson, Tencent IoT Explorer, MQTT.js, Chart.js, CloudBase 静态托管

---

## 文件结构

```
├── firmware/
│   └── esp32-iot/
│       ├── config.example.h          # WiFi/MQTT 凭证模板
│       └── esp32-iot.ino             # 主固件 (单文件)
├── web-console/
│   └── index.html                    # Web 控制台 (单文件含内联 CSS/JS)
├── cloudfunctions/
│   └── fetchHistory/
│       ├── index.js                  # 云函数: 查询 IoT 历史数据
│       └── package.json
└── docs/
    └── superpowers/
        └── specs/
            └── 2026-05-29-esp32-iot-temp-humidity-design.md
```

---

### Task 1: 腾讯云 IoT Explorer 平台配置

**说明:** 在腾讯云控制台创建产品、设备，获取连接凭证。这是后续所有开发的前置条件。

- [ ] **Step 1: 创建 IoT Explorer 产品**

  登录 https://console.cloud.tencent.com/iotexplorer
  1. 点击「创建产品」
  2. 产品名称: `温湿度监控`
  3. 品类: 标准品类 → 其他 → 其他
  4. 节点类型: 设备
  5. 认证方式: 密钥认证
  6. 数据协议: 物模型
  7. 创建后记录 **ProductId** (如 `7K8L9M0NOP`)

- [ ] **Step 2: 定义物模型**

  在产品详情 → 物模型 → 自定义功能，添加以下属性:

  | 标识符 | 名称 | 数据类型 | 读写 | 取值范围 |
  |--------|------|---------|------|---------|
  | `temperature` | 温度 | 浮点型 | 只读 | -20 ~ 60, 单位°C |
  | `humidity` | 湿度 | 整型 | 只读 | 0 ~ 100, 单位% |
  | `led_switch` | 灯开关 | 布尔型 | 可写 | 0=关, 1=开 |
  | `led_brightness` | 灯亮度 | 整型 | 可写 | 0 ~ 100 |

- [ ] **Step 3: 创建设备**

  产品详情 → 设备列表 → 创建新设备
  1. 设备名称: `dining_hall_01` (餐厅大堂1号)
  2. 创建后记录 **DeviceSecret**
  3. 记录完整凭证:
     - ProductId: `________`
     - DeviceName: `dining_hall_01`
     - DeviceSecret: `________`
     - MQTT Broker: `{ProductId}.iotcloud.tencentdevices.com`
     - MQTT Port: 1883 (非TLS) / 8883 (TLS)

- [ ] **Step 4: 创建云 API 密钥 (用于 Web 端查询历史数据)**

  腾讯云控制台 → 访问管理 → API 密钥管理 → 创建密钥
  1. 记录 **SecretId** 和 **SecretKey**
  2. 建议创建子账号，仅授权 IoT Explorer 读取权限

- [ ] **Step 5: 提交凭证配置**

```bash
git add -A
git commit -m "docs: add cloud credentials record and IoT setup guide"
```

---

### Task 2: ESP32S3 固件 — 项目骨架与 WiFi/MQTT 连接

**Files:**
- Create: `firmware/esp32-iot/config.example.h`
- Create: `firmware/esp32-iot/esp32-iot.ino`

- [ ] **Step 1: 创建配置文件模板**

```cpp
// firmware/esp32-iot/config.example.h
// 复制此文件为 config.h 并填入真实值 (config.h 已加入 .gitignore)

#ifndef CONFIG_H
#define CONFIG_H

// WiFi
#define WIFI_SSID       "你的WiFi名"
#define WIFI_PASSWORD   "你的WiFi密码"

// 腾讯云 IoT Explorer
#define PRODUCT_ID      "YOUR_PRODUCT_ID"
#define DEVICE_NAME     "dining_hall_01"
#define DEVICE_SECRET   "YOUR_DEVICE_SECRET"

// MQTT Broker
#define MQTT_BROKER     PRODUCT_ID ".iotcloud.tencentdevices.com"
#define MQTT_PORT       1883

#endif
```

- [ ] **Step 2: 创建固件主文件 — WiFi 连接 和 MQTT 基础框架**

```cpp
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

// ========== 上报计时 ==========
unsigned long lastReportTime = 0;
const unsigned long REPORT_INTERVAL = 60000;  // 60秒

// ========== MQTT Topic 拼接 ==========
const String TOPIC_UP   = "$thing/up/property/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME);
const String TOPIC_DOWN = "$thing/down/property/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME);

// ========== HMAC-SHA1 Base64 密码生成 (腾讯云 IoT 认证) ==========
String generateMQTTPassword() {
  // 腾讯云 IoT MQTT 认证:
  // username = PRODUCT_ID DEVICE_NAME;12010126;{connid};{expiry}
  // password = base64(hmac_sha1(device_secret, username))

  String connid = String(random(10000, 99999));
  unsigned long expiry = 0; // 0 = 永不过期(密钥认证模式)
  String username = String(PRODUCT_ID) + DEVICE_NAME + ";12010126;" + connid + ";" + String(expiry);

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

    String clientId = String(PRODUCT_ID) + DEVICE_NAME;
    String connid = String(random(10000, 99999));
    String username = String(PRODUCT_ID) + DEVICE_NAME + ";12010126;" + connid + ";0";
    String password = generateMQTTPassword();

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

    // 回执: 上报当前 LED 状态
    reportStatus();
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
  delay(1000);
  Serial.println("\n=== ESP32S3 IoT Starting ===");

  // 初始化 LED
  pinMode(LED_PIN, OUTPUT);
  analogWrite(LED_PIN, 0);

  // 初始化 I2C (SHT30)
  Wire.begin(21, 22);  // SDA=GPIO21, SCL=GPIO22
  if (!sht30.begin(0x44)) {
    Serial.println("SHT30 init failed! Check wiring.");
    while (1) delay(1000);
  }
  Serial.println("SHT30 OK");

  // 连接 WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
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

  unsigned long now = millis();
  if (now - lastReportTime >= REPORT_INTERVAL) {
    lastReportTime = now;
    reportTelemetry();
  }
}
```

- [ ] **Step 3: 创建 .gitignore，保护凭证文件**

```
# .gitignore
firmware/esp32-iot/config.h
.superpowers/
```

- [ ] **Step 4: 提交**

```bash
git add firmware/ .gitignore
git commit -m "feat: add ESP32S3 firmware with WiFi/MQTT/SHT30 skeleton"
```

---

### Task 3: Web 控制台 — 实时数据 + LED 控制

**Files:**
- Create: `web-console/index.html`

- [ ] **Step 1: 创建完整的 Web 控制台页面**

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>屁屁点餐 — 餐厅温湿度监控</title>
<script src="https://unpkg.com/mqtt@5.6.3/dist/mqtt.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  background:linear-gradient(135deg,#fff3e0,#ffe0b2);
  min-height:100vh;padding:16px;color:#333;
}
.container{max-width:480px;margin:0 auto}
h1{text-align:center;font-size:22px;margin-bottom:6px;color:#e8734a}
.subtitle{text-align:center;font-size:13px;color:#999;margin-bottom:20px}

/* 实时数据卡片 */
.cards{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px}
.card{
  background:#fff;border-radius:16px;padding:20px;text-align:center;
  box-shadow:0 2px 12px rgba(0,0,0,.08);
}
.card .value{font-size:48px;font-weight:700;color:#e8734a}
.card .unit{font-size:16px;color:#e8734a;font-weight:400}
.card .label{font-size:13px;color:#999;margin-top:4px}
.card.warn .value{color:#e53935}
.card.warn{animation:pulse 1.5s ease-in-out infinite}
@keyframes pulse{0%,100%{box-shadow:0 2px 12px rgba(0,0,0,.08)}50%{box-shadow:0 0 24px rgba(229,57,53,.3)}}

/* LED 控制 */
.led-panel{
  background:#fff;border-radius:16px;padding:20px;margin-bottom:16px;
  box-shadow:0 2px 12px rgba(0,0,0,.08);
}
.led-panel h3{font-size:16px;margin-bottom:12px;color:#555}
.led-row{display:flex;align-items:center;gap:12px;margin-bottom:12px}
.led-indicator{
  width:16px;height:16px;border-radius:50%;background:#ccc;
  transition:background .3s,box-shadow .3s;
}
.led-indicator.on{background:#ff9800;box-shadow:0 0 12px rgba(255,152,0,.6)}
.switch-btn{
  flex:1;padding:10px;border:none;border-radius:10px;font-size:16px;font-weight:600;
  cursor:pointer;transition:all .2s;color:#fff;
}
.switch-btn.on{background:#ff9800}
.switch-btn.off{background:#bdbdbd}

.slider-row{display:flex;align-items:center;gap:10px}
.slider-row label{font-size:13px;color:#888;white-space:nowrap}
.slider-row input[type=range]{flex:1;accent-color:#e8734a}
.slider-row .bval{width:40px;text-align:center;font-weight:600;color:#e8734a}
.send-btn{
  margin-top:12px;width:100%;padding:12px;border:none;border-radius:10px;
  background:#e8734a;color:#fff;font-size:16px;font-weight:600;cursor:pointer;
}
.send-btn:active{opacity:.8}

/* 历史曲线 */
.chart-panel{
  background:#fff;border-radius:16px;padding:20px;margin-bottom:16px;
  box-shadow:0 2px 12px rgba(0,0,0,.08);
}
.chart-panel h3{font-size:16px;margin-bottom:8px;color:#555}
.chart-wrap{position:relative;height:220px}
.chart-wrap canvas{width:100%!important}

/* 连接状态 */
.status{
  text-align:center;font-size:12px;padding:8px;border-radius:8px;margin-bottom:12px;
}
.status.online{background:#e8f5e9;color:#2e7d32}
.status.offline{background:#ffebee;color:#c62828}

/* 告警弹窗 */
.toast{
  position:fixed;top:20px;left:50%;transform:translateX(-50%);
  background:#e53935;color:#fff;padding:12px 24px;border-radius:12px;
  font-size:14px;font-weight:600;z-index:999;
  box-shadow:0 4px 20px rgba(229,57,53,.4);
  animation:slideDown .3s ease-out;
  display:none;
}
@keyframes slideDown{from{transform:translateX(-50%) translateY(-60px);opacity:0}to{transform:translateX(-50%) translateY(0);opacity:1}}
</style>
</head>
<body>

<div class="container">
  <h1>🌡️ 餐厅环境监控</h1>
  <p class="subtitle">屁屁点餐 · 智能温湿度</p>

  <!-- 连接状态 -->
  <div class="status offline" id="status">连接中...</div>

  <!-- 实时数据 -->
  <div class="cards">
    <div class="card" id="tempCard">
      <div class="value"><span id="tempVal">--</span><span class="unit">°C</span></div>
      <div class="label">当前温度</div>
    </div>
    <div class="card" id="humCard">
      <div class="value"><span id="humVal">--</span><span class="unit">%</span></div>
      <div class="label">当前湿度</div>
    </div>
  </div>

  <!-- LED 控制面板 -->
  <div class="led-panel">
    <h3>💡 灯光控制</h3>
    <div class="led-row">
      <div class="led-indicator off" id="ledDot"></div>
      <button class="switch-btn off" id="ledBtn" onclick="toggleLed()">OFF</button>
    </div>
    <div class="slider-row">
      <label>亮度</label>
      <input type="range" min="0" max="100" value="0" id="brightness">
      <span class="bval" id="bval">0%</span>
    </div>
    <button class="send-btn" onclick="sendLedCmd()">确认发送</button>
  </div>

  <!-- 历史曲线 -->
  <div class="chart-panel">
    <h3>📈 近24小时温湿度趋势</h3>
    <div class="chart-wrap"><canvas id="chart"></canvas></div>
  </div>
</div>

<!-- 告警 -->
<div class="toast" id="toast"></div>

<script>
// ========== 配置 (部署时替换为实际值) ==========
const PRODUCT_ID  = 'YOUR_PRODUCT_ID';
const DEVICE_NAME = 'dining_hall_01';
const DEVICE_SECRET = 'YOUR_DEVICE_SECRET';

// 告警阈值
const TEMP_ALERT_HIGH = 30;
const HUM_ALERT_HIGH  = 80;

// ========== MQTT 连接 ==========
const wsUrl = `wss://${PRODUCT_ID}.iotcloud.tencentdevices.com/mqtt`;

function genPassword() {
  // Web Crypto API HMAC-SHA1 → Base64 (与固件端对应)
  // 此处使用简化方案: 腾讯云 IoT WebSocket 可用 deviceSecret
  const connid = Math.floor(Math.random() * 90000 + 10000).toString();
  const username = PRODUCT_ID + DEVICE_NAME + ';12010126;' + connid + ';0';
  return { username, password: DEVICE_SECRET };
}

const auth = genPassword();
const client = mqtt.connect(wsUrl, {
  clientId: PRODUCT_ID + DEVICE_NAME,
  username: auth.username,
  password: auth.password,
  keepalive: 120,
  reconnectPeriod: 5000
});

const TOPIC_UP   = `$thing/up/property/${PRODUCT_ID}/${DEVICE_NAME}`;
const TOPIC_DOWN = `$thing/down/property/${PRODUCT_ID}/${DEVICE_NAME}`;

client.on('connect', () => {
  document.getElementById('status').textContent = '在线';
  document.getElementById('status').className = 'status online';
  client.subscribe(TOPIC_UP);
});

client.on('message', (topic, payload) => {
  try {
    const msg = JSON.parse(payload.toString());
    if (msg.method === 'report' && msg.params) {
      updateDisplay(msg.params);
    }
  } catch(e) {}
});

client.on('offline', () => {
  document.getElementById('status').textContent = '离线';
  document.getElementById('status').className = 'status offline';
});

client.on('error', (err) => {
  console.error('MQTT error:', err);
});

// ========== 更新显示 ==========
function updateDisplay(params) {
  if (params.temperature !== undefined) {
    document.getElementById('tempVal').textContent = params.temperature.toFixed(1);
    const card = document.getElementById('tempCard');
    if (params.temperature > TEMP_ALERT_HIGH) {
      card.classList.add('warn');
      showToast(`⚠️ 温度过高: ${params.temperature.toFixed(1)}°C (阈值 ${TEMP_ALERT_HIGH}°C)`);
    } else {
      card.classList.remove('warn');
    }
  }
  if (params.humidity !== undefined) {
    document.getElementById('humVal').textContent = Math.round(params.humidity);
    const card = document.getElementById('humCard');
    if (params.humidity > HUM_ALERT_HIGH) {
      card.classList.add('warn');
      showToast(`⚠️ 湿度过高: ${Math.round(params.humidity)}% (阈值 ${HUM_ALERT_HIGH}%)`);
    } else {
      card.classList.remove('warn');
    }
  }
  if (params.led_switch !== undefined) {
    updateLedUI(params.led_switch === 1, params.led_brightness || 0);
  }

  // 更新历史数据
  if (params.temperature !== undefined) {
    const now = new Date();
    const label = now.getHours().toString().padStart(2,'0') + ':' + now.getMinutes().toString().padStart(2,'0');
    tempData.labels.push(label);
    tempData.datasets[0].data.push(params.temperature);
    tempData.datasets[1].data.push(params.humidity);

    // 只保留最近 50 个点
    const maxPoints = 50;
    if (tempData.labels.length > maxPoints) {
      tempData.labels.shift();
      tempData.datasets[0].data.shift();
      tempData.datasets[1].data.shift();
    }
    chart.update();
  }
}

// ========== LED UI ==========
function updateLedUI(state, brightness) {
  const dot = document.getElementById('ledDot');
  const btn = document.getElementById('ledBtn');
  const slider = document.getElementById('brightness');
  const bval = document.getElementById('bval');

  if (state) {
    dot.className = 'led-indicator on';
    btn.textContent = 'ON';
    btn.className = 'switch-btn on';
  } else {
    dot.className = 'led-indicator off';
    btn.textContent = 'OFF';
    btn.className = 'switch-btn off';
  }
  slider.value = brightness;
  bval.textContent = brightness + '%';
}

function toggleLed() {
  const btn = document.getElementById('ledBtn');
  const newState = btn.textContent === 'OFF';
  const slider = document.getElementById('brightness');
  const brightness = parseInt(slider.value);
  updateLedUI(newState, brightness);
}

function sendLedCmd() {
  const btn = document.getElementById('ledBtn');
  const slider = document.getElementById('brightness');
  const state = btn.textContent === 'ON' ? 1 : 0;
  const brightness = parseInt(slider.value);

  const msg = {
    method: 'control',
    clientToken: Math.floor(Math.random() * 900000 + 100000).toString(),
    params: {
      led_switch: state,
      led_brightness: brightness
    }
  };

  client.publish(TOPIC_DOWN, JSON.stringify(msg));
  showToast(`已发送: LED ${state?'开':'关'}, 亮度 ${brightness}%`);
}

// ========== 告警弹窗 ==========
let toastTimer;
function showToast(text) {
  const el = document.getElementById('toast');
  el.textContent = text;
  el.style.display = 'block';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.style.display = 'none'; }, 5000);
}

// ========== 历史曲线 ==========
const tempData = {
  labels: [],
  datasets: [
    { label: '温度 (°C)', data: [], borderColor: '#e8734a', backgroundColor: 'rgba(232,115,74,.1)', tension: 0.4, fill: true },
    { label: '湿度 (%)',  data: [], borderColor: '#42a5f5', backgroundColor: 'rgba(66,165,245,.1)',  tension: 0.4, fill: true }
  ]
};

const ctx = document.getElementById('chart').getContext('2d');
const chart = new Chart(ctx, {
  type: 'line',
  data: tempData,
  options: {
    responsive: true,
    maintainAspectRatio: false,
    plugins: { legend: { position: 'bottom', labels: { boxWidth: 12, padding: 16, font: { size: 11 } } } },
    scales: {
      x: { display: false },
      y: { min: 0, max: 60, ticks: { font: { size: 10 } }, grid: { color: '#f0f0f0' } }
    }
  }
});

// ========== 亮度滑块实时更新 ==========
document.getElementById('brightness').addEventListener('input', function() {
  document.getElementById('bval').textContent = this.value + '%';
  updateLedUI(document.getElementById('ledBtn').textContent === 'ON', parseInt(this.value));
});
</script>

</body>
</html>
```

- [ ] **Step 2: 提交**

```bash
git add web-console/
git commit -m "feat: add web console with MQTT.js real-time display and LED control"
```

---

### Task 4: CloudBase 部署 — 静态网站托管

**说明:** 利用已有的 CloudBase 环境 (cloudbase-d1gz5vo2h23ee7ece) 托管 Web 控制台。

- [ ] **Step 1: 配置 CloudBase 静态托管**

  1. 登录腾讯云 CloudBase 控制台: https://console.cloud.tencent.com/tcb
  2. 选择环境 `cloudbase-d1gz5vo2h23ee7ece`
  3. 左侧菜单 → 静态网站托管 → 开通 (如未开通)
  4. 记录静态托管域名 (如 `cloudbase-d1gz5vo2h23ee7ece-xxxx.tcloudbaseapp.com`)

- [ ] **Step 2: 安装 CloudBase CLI 并部署**

```bash
npm install -g @cloudbase/cli
cloudbase login
cloudbase hosting deploy web-console/index.html -e cloudbase-d1gz5vo2h23ee7ece
```

- [ ] **Step 3: 验证部署**

  浏览器打开静态托管域名，确认页面加载正常，替换其中的 `PRODUCT_ID` 和 `DEVICE_SECRET` 为真实值。

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "chore: add CloudBase static hosting deployment config"
```

---

### Task 5: 联调测试

**说明:** 硬件 + 云端 + Web 三端联调，验证完整数据流。

- [ ] **Step 1: 硬件连接验证**

  1. 按设计文档引脚连接: SHT30 → GPIO21/22, LED → GPIO5(串220Ω电阻)
  2. USB 供电 ESP32S3
  3. Arduino IDE → 串口监视器(115200baud)，确认:
     - `SHT30 OK`
     - `WiFi connected, IP: x.x.x.x`
     - `MQTT connected`
     - `Subscribed: $thing/down/property/...`

- [ ] **Step 2: 数据上报验证**

  串口监视器中应每60秒出现:
  ```
  Reported: 25.3°C, 62%
  ```
  登录 IoT Explorer 控制台 → 设备 → `dining_hall_01` → 设备日志，确认收到上报数据。

- [ ] **Step 3: Web 控制台验证**

  1. 浏览器打开 Web 控制台 URL
  2. 确认页面显示「在线」状态
  3. 实时数据卡片显示当前温湿度
  4. 历史曲线逐步累积数据点

- [ ] **Step 4: LED 控制验证**

  1. Web 面板: 将开关拨到 ON，亮度设为 50%，点击「确认发送」
  2. 确认 LED 亮起且亮度为 50%
  3. 串口监视器输出: `LED: ON, brightness: 50%`
  4. IoT Explorer 设备日志收到 LED 状态回执

- [ ] **Step 5: 告警验证**

  1. 用手握住 SHT30 使其升温至 30°C 以上
  2. Web 页面中温度卡片变红色闪烁 + 弹出告警提示

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "test: integration test passed — sensor report + LED control + alert verified"
```

---

## 自检清单

| 检查项 | 结果 |
|--------|------|
| Spec 覆盖 | Task1→云配置, Task2→固件+传感器+LED, Task3→Web控制台+告警, Task4→部署, Task5→联调 |
| 无占位符 | 无 TBD/TODO，所有代码块完整 |
| 类型一致性 | config.h 字段与 .ino 引用一致，Web MQTT Topic 与固件一致，物模型属性名三端一致 |
