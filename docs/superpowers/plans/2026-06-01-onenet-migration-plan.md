# OneNET + GitHub Pages 迁移实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 ESP32S3 温湿度 IoT 系统从腾讯云 IoT Explorer 迁移到 OneNET（免费）+ GitHub Pages（免费）

**Architecture:** ESP32S3 通过 MQTT 物模型协议上报温湿度 + LED 状态到 OneNET，订阅下行 Topic 接收 LED 控制指令。Web 控制台通过 MQTT.js WebSocket 直连 OneNET 实时显示数据，通过 OneNET HTTP API 查询历史曲线，部署到 GitHub Pages。

**Tech Stack:** Arduino C++ (ESP32S3), PubSubClient, Adafruit SHT31, ArduinoJson, OneNET IoT Platform, MQTT.js, Chart.js, GitHub Pages

---

## 文件结构

```
├── firmware/esp32-iot/
│   ├── config.example.h    # 修改: 参数替换
│   └── esp32-iot.ino       # 修改: 认证/Topic/JSON
└── web-console/
    └── index.html           # 修改: MQTT连接/Topic/JSON/历史API
```

---

### Task 1: OneNET 平台配置

**说明:** 在 OneNET 控制台创建产品、设备和物模型，获取连接凭证和 API 密钥。

- [ ] **Step 1: 注册 OneNET 账号并创建产品**

  1. 打开 https://open.iot.10086.cn/
  2. 注册/登录账号
  3. 控制台 → 产品管理 → 创建产品
  4. 填写产品信息：
     - 产品名称: `温湿度监控`
     - 协议: **MQTT**
     - 节点类型: **直连设备**
     - 联网方式: **Wi-Fi**
  5. 创建后进入产品详情，记录 **产品ID (product_id)**

- [ ] **Step 2: 定义物模型**

  产品详情 → 物模型 → 添加自定义属性：

  | 功能类型 | 标识符 | 名称 | 数据类型 | 读写 | 取值范围 |
  |---------|--------|------|---------|------|---------|
  | 属性 | `temperature` | 温度 | float | 只读 | -20 ~ 60, 单位°C |
  | 属性 | `humidity` | 湿度 | int | 只读 | 0 ~ 100, 单位% |
  | 属性 | `led_switch` | 灯开关 | bool | 可写 | 0=关, 1=开 |
  | 属性 | `led_brightness` | 灯亮度 | int | 可写 | 0 ~ 100 |

- [ ] **Step 3: 创建设备**

  1. 产品详情 → 设备管理 → 添加设备
  2. 设备名称: `dining_hall_01`
  3. 认证方式: **设备密钥**
  4. 创建成功后立即**复制并保存 device_key**（仅显示一次！）

- [ ] **Step 4: 获取 API 访问密钥**

  1. 控制台右上角头像 → 访问权限 → 新建密钥
  2. 权限选择: 设备管理（读写）
  3. 创建后记录 **access_key**

- [ ] **Step 5: 整理凭证**

  ```
  PRODUCT_ID  = "_________"   # 产品ID
  DEVICE_NAME = "dining_hall_01"
  DEVICE_KEY  = "_________"   # 设备密钥 (设备创建时显示)
  ACCESS_KEY  = "_________"   # API 访问密钥 (访问权限页面创建)

  MQTT Broker = PRODUCT_ID ".st1.iot.iotdas.10086.cn:1883"
  WebSocket   = "mqtts.heclouds.com:8083/mqtt"
  HTTP API    = "https://iot-api.heclouds.com"
  ```

- [ ] **Step 6: 提交凭证信息**

```bash
git add -A
git commit -m "docs: add OneNET cloud credentials record and setup guide"
```

---

### Task 2: 固件改造 — 从腾讯云 IoT 迁到 OneNET

**Files:**
- Modify: `firmware/esp32-iot/config.example.h`
- Modify: `firmware/esp32-iot/esp32-iot.ino`

- [ ] **Step 1: 更新配置文件**

  用以下内容替换 `firmware/esp32-iot/config.example.h`：

```cpp
// firmware/esp32-iot/config.example.h
// 复制此文件为 config.h 并填入真实值 (config.h 已加入 .gitignore)

#ifndef CONFIG_H
#define CONFIG_H

// WiFi
#define WIFI_SSID       "你的WiFi名"
#define WIFI_PASSWORD   "你的WiFi密码"

// OneNET 物联网开放平台
#define PRODUCT_ID      "YOUR_PRODUCT_ID"
#define DEVICE_NAME     "dining_hall_01"
#define DEVICE_KEY      "YOUR_DEVICE_KEY"

// MQTT Broker
#define MQTT_BROKER     PRODUCT_ID ".st1.iot.iotdas.10086.cn"
#define MQTT_PORT       1883

#endif
```

- [ ] **Step 2: 移除 mbedtls 头文件引用**

  在 `esp32-iot.ino` 中删除第 8 行：

```cpp
// 删除这行:
#include <mbedtls/md.h>
```

- [ ] **Step 3: 删除 HMAC-SHA1 密码生成函数**

  删除 `generateMQTTPassword()` 整个函数（第 33-62 行）。

- [ ] **Step 4: 替换 Topic 定义**

  将第 26-28 行的 Topic 定义替换为：

```cpp
// ========== MQTT Topic 拼接 ==========
const String TOPIC_POST  = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post";
const String TOPIC_SET   = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/set";
const String TOPIC_REPLY = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post/reply";
```

- [ ] **Step 5: 重写 reconnectMQTT() 函数**

  用以下代码替换第 64-85 行的 `reconnectMQTT()` 函数：

```cpp
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
```

- [ ] **Step 6: 更新 mqttCallback() — OneNET 物模型嵌套 JSON 解析**

  用以下代码替换第 87-114 行的 `mqttCallback()` 函数：

```cpp
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
```

- [ ] **Step 7: 更新 reportTelemetry() — OneNET 物模型嵌套 JSON 上报**

  用以下代码替换第 117-143 行的 `reportTelemetry()` 函数：

```cpp
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
```

- [ ] **Step 8: 更新 reportStatus() — 上报 LED 状态回执**

  用以下代码替换第 145-157 行的 `reportStatus()` 函数：

```cpp
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
```

- [ ] **Step 9: 验证编译**

  在 Arduino IDE 中打开 `esp32-iot.ino`，选择 ESP32S3 开发板，点击"验证"编译。

  预期: 编译通过，无报错。

- [ ] **Step 10: 提交固件改造**

```bash
git add firmware/esp32-iot/config.example.h firmware/esp32-iot/esp32-iot.ino
git commit -m "refactor: migrate firmware from Tencent IoT to OneNET (auth/topic/JSON)"
```

---

### Task 3: Web 控制台改造 + GitHub Pages 部署

**Files:**
- Modify: `web-console/index.html`

- [ ] **Step 1: 更新配置参数**

  将 `index.html` 中第 137-140 行的配置部分替换为：

```js
// ========== 配置 (部署时替换为实际值) ==========
const PRODUCT_ID  = 'YOUR_PRODUCT_ID';
const DEVICE_NAME = 'dining_hall_01';
const DEVICE_KEY  = 'YOUR_DEVICE_KEY';
const ACCESS_KEY  = 'YOUR_ACCESS_KEY';

// 告警阈值
const TEMP_ALERT_HIGH = 30;
const HUM_ALERT_HIGH  = 80;
```

- [ ] **Step 2: 替换 MQTT 连接和 genPassword**

  将第 146-166 行的 MQTT 连接代码替换为：

```js
// ========== MQTT 连接 (OneNET WebSocket) ==========
const wsUrl = `wss://mqtts.heclouds.com:8083/mqtt`;

const client = mqtt.connect(wsUrl, {
  clientId: DEVICE_NAME,
  username: PRODUCT_ID,
  password: DEVICE_KEY,
  keepalive: 120,
  reconnectPeriod: 5000
});
```

  移除旧的 `genPassword()` 函数和 `auth` 变量。

- [ ] **Step 3: 替换 Topic 定义**

  将第 164-165 行的 Topic 替换为：

```js
// ========== OneNET Topic ==========
const TOPIC_POST = `$sys/${PRODUCT_ID}/${DEVICE_NAME}/thing/property/post`;  // 订阅: 收设备上报
const TOPIC_SET  = `$sys/${PRODUCT_ID}/${DEVICE_NAME}/thing/property/set`;   // 发布: 发LED指令
```

- [ ] **Step 4: 更新 connect 回调中的订阅 Topic**

  将第 167-171 行的 connect 回调中 `client.subscribe(TOPIC_UP)` 改为：

```js
client.on('connect', () => {
  document.getElementById('status').textContent = '在线';
  document.getElementById('status').className = 'status online';
  client.subscribe(TOPIC_POST);
});
```

- [ ] **Step 5: 更新 updateDisplay() — OneNET 物模型嵌套 JSON 解析**

  用以下代码替换第 192-236 行的 `updateDisplay()` 函数：

```js
// ========== 更新显示 ==========
function updateDisplay(params) {
  // OneNET 物模型嵌套格式: {"temperature": {"value": 25.3}}
  const tempVal = params.temperature?.value;
  const humVal  = params.humidity?.value;
  const temp = typeof tempVal === 'number' ? tempVal : parseFloat(tempVal);
  const hum  = typeof humVal === 'number' ? humVal : parseFloat(humVal);

  if (tempVal !== undefined && !isNaN(temp)) {
    document.getElementById('tempVal').textContent = temp.toFixed(1);
    const card = document.getElementById('tempCard');
    if (temp > TEMP_ALERT_HIGH) {
      card.classList.add('warn');
      showToast(`⚠️ 温度过高: ${temp.toFixed(1)}°C (阈值 ${TEMP_ALERT_HIGH}°C)`);
    } else {
      card.classList.remove('warn');
    }
  }
  if (humVal !== undefined && !isNaN(hum)) {
    document.getElementById('humVal').textContent = Math.round(hum);
    const card = document.getElementById('humCard');
    if (hum > HUM_ALERT_HIGH) {
      card.classList.add('warn');
      showToast(`⚠️ 湿度过高: ${Math.round(hum)}% (阈值 ${HUM_ALERT_HIGH}%)`);
    } else {
      card.classList.remove('warn');
    }
  }
  if (params.led_switch) {
    updateLedUI(params.led_switch.value === 1, params.led_brightness?.value || 0);
  }

  // 更新历史数据
  if (tempVal !== undefined && !isNaN(temp)) {
    const now = new Date();
    const label = now.getHours().toString().padStart(2,'0') + ':' + now.getMinutes().toString().padStart(2,'0');
    tempData.labels.push(label);
    tempData.datasets[0].data.push(temp);
    tempData.datasets[1].data.push(hum);

    const maxPoints = 50;
    if (tempData.labels.length > maxPoints) {
      tempData.labels.shift();
      tempData.datasets[0].data.shift();
      tempData.datasets[1].data.shift();
    }
    chart.update();
  }
}
```

- [ ] **Step 6: 更新 sendLedCmd() — OneNET 物模型下行格式**

  用以下代码替换第 266-283 行的 `sendLedCmd()` 函数：

```js
function sendLedCmd() {
  const btn = document.getElementById('ledBtn');
  const slider = document.getElementById('brightness');
  const state = btn.textContent === 'ON' ? 1 : 0;
  const brightness = parseInt(slider.value);

  const msg = {
    id: Date.now().toString(),
    params: {
      led_switch:      { value: state },
      led_brightness:  { value: brightness }
    }
  };

  client.publish(TOPIC_SET, JSON.stringify(msg));
  showToast(`已发送: LED ${state?'开':'关'}, 亮度 ${brightness}%`);
}
```

- [ ] **Step 7: 新增历史数据 HTTP API 查询**

  在 `// ========== 历史曲线 ==========` 注释之前（第 295 行之前），插入以下代码：

```js
// ========== 历史数据 (OneNET HTTP API) ==========
async function fetchHistory() {
  const end   = new Date();
  const start = new Date(end.getTime() - 24 * 60 * 60 * 1000);
  const url = `https://iot-api.heclouds.com/thing-history/${PRODUCT_ID}/${DEVICE_NAME}` +
    `?start=${start.toISOString()}&end=${end.toISOString()}&limit=500`;

  try {
    const res = await fetch(url, {
      headers: { 'Authorization': ACCESS_KEY }
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    if (data.items && data.items.length > 0) {
      // 清空已有的初始数据
      tempData.labels = [];
      tempData.datasets[0].data = [];
      tempData.datasets[1].data = [];

      data.items.forEach(item => {
        const ts = new Date(item.timestamp);
        const label = ts.getHours().toString().padStart(2,'0') + ':' +
                      ts.getMinutes().toString().padStart(2,'0');
        tempData.labels.push(label);
        tempData.datasets[0].data.push(item.temperature?.value);
        tempData.datasets[1].data.push(item.humidity?.value);
      });
      chart.update();
    }
  } catch (e) {
    console.error('Failed to fetch history:', e);
  }
}
```

- [ ] **Step 8: 页面加载时调用 fetchHistory()**

  在 `</script>` 之前（第 338 行之前），Chart.js 初始化代码之后，添加：

```js
// ========== 页面加载: 查询历史数据 ==========
window.addEventListener('DOMContentLoaded', () => {
  fetchHistory();
});
```

- [ ] **Step 9: 本地测试**

  1. 在 `index.html` 中填入真实的 `PRODUCT_ID`、`DEVICE_KEY`、`ACCESS_KEY`
  2. 用浏览器打开 `index.html`
  3. 预期: 页面显示"在线"状态，历史数据从 OneNET API 加载并渲染曲线
  4. LED 开关 + 亮度滑块功能正常
  5. 注意: 如果没有 ESP32S3 在线发送数据，历史数据可能为空，但页面不报错

- [ ] **Step 10: 部署到 GitHub Pages**

```bash
# 1. 在 GitHub 创建仓库 (如 esp32-iot-console)
# 2. 初始化 git 并推送
cd web-console
git init
git add index.html
git commit -m "feat: web console migrated to OneNET + GitHub Pages"
git remote add origin https://github.com/YOUR_USERNAME/esp32-iot-console.git
git push -u origin main

# 3. GitHub 仓库 → Settings → Pages → Source: main branch → Save
# 4. 等待几分钟，访问 https://YOUR_USERNAME.github.io/esp32-iot-console/
```

- [ ] **Step 11: 提交 Web 控制台改造**

```bash
git add web-console/index.html
git commit -m "refactor: migrate web console from Tencent IoT to OneNET, add history API query"
```

---

## 自检清单

| 检查项 | 结果 |
|--------|------|
| Spec 覆盖 | Task1→OneNET平台配置, Task2→固件改造(认证/Topic/JSON), Task3→Web改造(MQTT连接/JSON/历史API/GitHub Pages) |
| 无占位符 | 所有代码块完整，参数标注 `YOUR_XXX` 为须填入的真实值 |
| 类型一致性 | 物模型属性名三端一致: temperature/humidity/led_switch/led_brightness，JSON嵌套格式 `{"value": ...}` 一致 |
| Topic 一致性 | 固件端 TOPIC_POST/SET/REPLY 与 Web 端 TOPIC_POST/SET 格式一致 |
