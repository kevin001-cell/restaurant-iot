# ESP32S3 温湿度监控 — 迁移到 OneNET 平台设计方案

**日期:** 2026-06-01
**状态:** 已确认
**前置:** 原方案基于腾讯云 IoT Explorer（见 [2026-05-29-esp32-iot-temp-humidity-design.md](2026-05-29-esp32-iot-temp-humidity-design.md)），本方案将云平台替换为 OneNET + GitHub Pages 免费方案

## 1. 迁移背景

腾讯云 IoT Explorer 需要付费，OneNET（中国移动物联网开放平台）免费提供：MQTT Broker、设备管理、物模型、时序数据存储、HTTP 查询 API。Web 控制台从 CloudBase 托管改为 GitHub Pages 免费托管。

## 2. 系统架构

```
┌─────────────────────────────────────────────────────────┐
│              中国移动 OneNET 物联网开放平台 (免费)             │
│  MQTT Broker (TLS) │ 设备管理 │ 物模型 │ 时序存储 │ HTTP API  │
└──────┬───────────────────────────────────────────────────┘
       │ MQTT (固件)         │ WebSocket + HTTP API (浏览器)
       ▼                     ▼
  ┌──────────────┐    ┌────────────────────┐
  │  ESP32S3     │    │  Web 控制台          │
  │  SHT30 采集   │    │  MQTT.js 实时订阅    │
  │  PWM 控 LED   │    │  OneNET API 查历史   │
  │  60s 上报     │    │  GitHub Pages 部署    │
  └──────────────┘    └────────────────────┘
```

### 数据流

1. **上报**: ESP32S3 → MQTT publish `$sys/{pid}/{dev}/thing/property/post` → OneNET 存储 → Web 通过 MQTT.js WebSocket 订阅实时接收
2. **控制**: Web → MQTT publish `$sys/{pid}/{dev}/thing/property/set` → OneNET 转发 → ESP32S3 subscribe 接收 → GPIO/PWM 执行
3. **历史**: Web 调用 OneNET HTTP API 查询时序数据库，渲染历史曲线
4. **告警**: Web 端判断阈值（温度>30°C 或湿度>80%），页面弹窗提醒

## 3. OneNET 平台配置

### 产品创建参数

| 配置项 | 选择 |
|--------|------|
| 协议 | MQTT |
| 节点类型 | 直连设备 |
| 认证方式 | 设备密钥 (device_key) |
| 物模型 | 启用 |

创建后获取：`product_id`、`device_name`、`device_key`、`access_key`（API 查询用）

### MQTT 连接地址

```
非 TLS:  {product_id}.st1.iot.iotdas.10086.cn:1883
TLS:     mqtts.heclouds.com:8883
WebSocket TLS: mqtts.heclouds.com:8083
```

### MQTT 认证

```
username  = product_id
password  = device_key
clientId  = device_name
```

无需 HMAC-SHA1 签名计算，认证大幅简化。

### 物模型属性定义

| 标识符 | 名称 | 数据类型 | 读写 | 取值范围 |
|--------|------|---------|------|---------|
| `temperature` | 温度 | float | 只读 | -20 ~ 60, 单位°C |
| `humidity` | 湿度 | int | 只读 | 0 ~ 100, 单位% |
| `led_switch` | 灯开关 | bool | 可写 | 0=关, 1=开 |
| `led_brightness` | 灯亮度 | int | 可写 | 0 ~ 100 |

### Topic 格式 (物模型模式)

| Topic | 方向 | 说明 |
|-------|------|------|
| `$sys/{pid}/{dev}/thing/property/post` | 设备→云 | 属性上报 |
| `$sys/{pid}/{dev}/thing/property/set` | 云→设备 | 属性下发（LED 控制） |
| `$sys/{pid}/{dev}/thing/property/post/reply` | 云→设备 | 上报回执（可选） |

### JSON 格式

**属性上报:**
```json
{
  "id": "123456",
  "params": {
    "temperature":     {"value": 25.3},
    "humidity":        {"value": 62},
    "led_switch":      {"value": 0},
    "led_brightness":  {"value": 60}
  }
}
```

**属性下发 (LED 控制):**
```json
{
  "id": "123456",
  "params": {
    "led_switch":      {"value": 1},
    "led_brightness":  {"value": 60}
  }
}
```

### HTTP API 查询历史数据

```
GET https://iot-api.heclouds.com/thing-history/{product_id}/{device_name}
    ?start=2026-06-01T00:00:00&end=2026-06-01T23:59:59
Headers: Authorization: {access_key}
```

## 4. 固件改造 (Arduino/C++)

### 改动文件

`firmware/esp32-iot/config.example.h` — 参数替换
`firmware/esp32-iot/esp32-iot.ino` — 认证/Topic/JSON 格式改造

### 4.1 配置参数变化

```cpp
// 旧
#define DEVICE_SECRET   "YOUR_DEVICE_SECRET"
#define MQTT_BROKER     PRODUCT_ID ".iotcloud.tencentdevices.com"
#define MQTT_PORT       1883

// 新
#define DEVICE_KEY      "YOUR_DEVICE_KEY"              // OneNET 设备密钥
#define MQTT_BROKER     PRODUCT_ID ".st1.iot.iotdas.10086.cn"
#define MQTT_PORT       1883
```

WiFi 配置、DEVICE_NAME 不变。

### 4.2 删除 HMAC-SHA1 认证

删除 `generateMQTTPassword()` 整个函数（约 40 行），`reconnectMQTT()` 改为：

```cpp
void reconnectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("MQTT connecting...");
    String clientId = DEVICE_NAME;
    // OneNET 认证: username=product_id, password=device_key
    if (mqtt.connect(clientId.c_str(), PRODUCT_ID, DEVICE_KEY)) {
      Serial.println("connected");
      mqtt.subscribe(TOPIC_SET.c_str());
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

### 4.3 Topic 替换

```cpp
const String TOPIC_POST  = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post";
const String TOPIC_SET   = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/set";
const String TOPIC_REPLY = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post/reply";
```

需要 `#include <mbedtls/md.h>` 可以删掉。

### 4.4 JSON 格式适配

```cpp
void reportTelemetry() {
  sht30.readBoth();
  float temp = sht30.readTemperature();
  float hum  = sht30.readHumidity();
  if (isnan(temp) || isnan(hum)) {
    Serial.println("SHT30 read error");
    return;
  }

  StaticJsonDocument<256> doc;
  char id[16]; snprintf(id, 16, "%lu", millis());
  doc["id"] = id;
  JsonObject params = doc.createNestedObject("params");
  params.createNestedObject("temperature")["value"]     = round(temp * 10) / 10.0;
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

下发消息解析:

```cpp
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.concat((char*)payload, length);
  Serial.print("Received: ");
  Serial.println(msg);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) return;

  JsonObject params = doc["params"];
  if (params.containsKey("led_switch"))
    ledState = params["led_switch"]["value"].as<int>() == 1;
  if (params.containsKey("led_brightness"))
    ledBrightness = params["led_brightness"]["value"].as<int>();

  int pwmValue = ledState ? map(ledBrightness, 0, 100, 0, 255) : 0;
  analogWrite(LED_PIN, pwmValue);
  Serial.printf("LED: %s, brightness: %d%%\n", ledState ? "ON" : "OFF", ledBrightness);

  statusReportPending = true;
}
```

### 4.5 不变的部分

- SHT30 传感器初始化和读取逻辑
- LED PWM 控制和状态管理
- WiFi 连接与超时重启
- 60 秒上报间隔
- `statusReportPending` 延迟上报机制
- PubSubClient、Adafruit_SHT31、ArduinoJson 库

## 5. Web 控制台改造

### 改动文件

`web-console/index.html` — MQTT 连接/Topic/JSON 解析改造，新增 HTTP API 历史数据查询

### 5.1 MQTT 连接变化

```js
// 旧 (腾讯云)
const wsUrl = `wss://${PRODUCT_ID}.iotcloud.tencentdevices.com/mqtt`;
const auth = genPassword();
const client = mqtt.connect(wsUrl, {
  clientId: PRODUCT_ID + DEVICE_NAME,
  username: auth.username,
  password: auth.password,
});

// 新 (OneNET WebSocket)
const wsUrl = `wss://mqtts.heclouds.com:8083/mqtt`;
const client = mqtt.connect(wsUrl, {
  clientId: DEVICE_NAME,
  username: PRODUCT_ID,
  password: DEVICE_KEY,
});
```

不再需要 `genPassword()` 函数。

### 5.2 配置参数

```js
const PRODUCT_ID  = 'YOUR_PRODUCT_ID';
const DEVICE_NAME = 'dining_hall_01';
const DEVICE_KEY  = 'YOUR_DEVICE_KEY';
const ACCESS_KEY  = 'YOUR_ACCESS_KEY';   // 新增: OneNET HTTP API 访问密钥
```

### 5.3 Topic 订阅变化

```js
// 订阅设备上报 (接收实时数据)
const TOPIC_POST = `$sys/${PRODUCT_ID}/${DEVICE_NAME}/thing/property/post`;
// 发布 LED 控制指令
const TOPIC_SET  = `$sys/${PRODUCT_ID}/${DEVICE_NAME}/thing/property/set`;

client.on('connect', () => {
  client.subscribe(TOPIC_POST);
});
```

### 5.4 JSON 解析适配

```js
function updateDisplay(params) {
  // OneNET 物模型嵌套格式: {"温度": {"value": 25.3}}
  const temp = params.temperature?.value;
  const hum  = params.humidity?.value;

  if (temp !== undefined && !isNaN(temp)) {
    document.getElementById('tempVal').textContent = temp.toFixed(1);
    // 告警判断...
    // 追加历史数据点...
  }
  if (hum !== undefined && !isNaN(hum)) {
    document.getElementById('humVal').textContent = Math.round(hum);
    // 告警判断...
  }
  if (params.led_switch) {
    updateLedUI(params.led_switch.value === 1, params.led_brightness?.value || 0);
  }
}
```

### 5.5 🆕 历史数据 HTTP API 查询

页面加载时查询最近 24 小时数据：

```js
async function fetchHistory() {
  const end   = new Date();
  const start = new Date(end.getTime() - 24 * 60 * 60 * 1000);
  const url = `https://iot-api.heclouds.com/thing-history/${PRODUCT_ID}/${DEVICE_NAME}` +
    `?start=${start.toISOString()}&end=${end.toISOString()}`;

  try {
    const res = await fetch(url, { headers: { 'Authorization': ACCESS_KEY } });
    const data = await res.json();
    if (data.items) {
      data.items.forEach(item => {
        tempData.labels.push(formatTime(item.timestamp));
        tempData.datasets[0].data.push(item.temperature?.value);
        tempData.datasets[1].data.push(item.humidity?.value);
      });
      chart.update();
    }
  } catch(e) {
    console.error('Failed to fetch history:', e);
  }
}

// 页面加载时调用
fetchHistory();
```

### 5.6 部署：GitHub Pages

- 将 `index.html` 上传到 GitHub 仓库 `<username>.github.io` 或任意 repo 的 `gh-pages` 分支
- 设置 → Pages → 选择分支 → 保存
- HTTPS 免费自带
- 每次 `git push` 自动部署

### 5.7 不变的部分

- Chart.js 双轴曲线
- LED 开关按钮 + 亮度滑块
- 告警弹窗（温度>30°C / 湿度>80%）
- 整体 UI 风格（橙色主题）

## 6. 开发步骤

| # | 任务 | 说明 |
|---|------|------|
| Task 1 | OneNET 平台配置 | 注册账号、创建产品、定义物模型、创建设备、获取 device_key 和 access_key |
| Task 2 | 固件改造 | 替换认证/Topic/JSON，删除 HMAC-SHA1，测试编译通过 |
| Task 3 | Web 控制台改造 | 替换 MQTT 连接/Topic/JSON，新增历史数据 API 查询，部署到 GitHub Pages |

## 7. 改动文件汇总

| 文件 | 改动类型 | 改动量 |
|------|---------|--------|
| `firmware/esp32-iot/config.example.h` | 修改 | 小 — 参数名替换 |
| `firmware/esp32-iot/esp32-iot.ino` | 修改 | 中 — 删 HMAC 函数，改 Topic/JSON |
| `web-console/index.html` | 修改 | 中 — 改 MQTT 连接/JSON，加 HTTP API 查询 |

无需新建文件，无需新增依赖库。

## 8. 与旧方案对比

| 维度 | 旧方案 (腾讯云 IoT) | 新方案 (OneNET) |
|------|----------------------|-----------------|
| 云平台 | 腾讯云 IoT Explorer (付费) | OneNET (免费) |
| 认证方式 | HMAC-SHA1 签名 (复杂) | 设备密钥 明文 (简化) |
| 时序存储 | 腾讯云 IoT 数据库 | OneNET 内置 (免费) |
| 历史查询 | 云函数 + SDK | 原生 HTTP API |
| Web 托管 | CloudBase 静态托管 (付费) | GitHub Pages (免费) |
| Topic 格式 | `$thing/up|down/property/{pid}/{dev}` | `$sys/{pid}/{dev}/thing/property/post|set` |
| JSON 格式 | `{"temperature": 25.3}` (扁平) | `{"temperature":{"value":25.3}}` (嵌套) |
| 国内速度 | 腾讯云 CDN | 中国移动 CDN |
| 总体费用 | 按量付费 | **零费用** |
