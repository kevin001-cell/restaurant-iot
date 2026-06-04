# Public MQTT Broker Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a public MQTT broker (`broker.emqx.io`) as a real-time relay between ESP32 and the web console, bypassing OneNET CORS/WebSocket limitations.

**Architecture:** ESP32 maintains two MQTT connections — one to OneNET (data storage) and one to broker.emqx.io (real-time relay). The web console connects to broker.emqx.io via WebSocket for live telemetry and LED control. OneNET continues to serve as the authoritative data store; the public broker handles only live streaming.

**Tech Stack:** Arduino C++ (PubSubClient, ArduinoJson), browser JavaScript (MQTT.js, Chart.js)

**Files:**
- Modify: `firmware/esp32-iot/config.example.h` — add public broker config
- Modify: `firmware/esp32-iot/config.h` — add public broker values  
- Modify: `firmware/esp32-iot/esp32-iot.ino` — dual MQTT, telemetry push, control subscribe
- Modify: `web-console/index.html` — WebSocket connect, subscribe, publish, remove polling

---

### Task 1: Add public broker config to firmware

**Files:**
- Modify: `firmware/esp32-iot/config.example.h`
- Modify: `firmware/esp32-iot/config.h`

- [ ] **Step 1: Add public broker defines to config.example.h**

Add after the existing `MQTT_PORT` define:

```cpp
// 公共 MQTT Broker (用于 Web 控制台实时通信)
#define PUB_BROKER      "broker.emqx.io"
#define PUB_PORT        1883
#define PUB_TOPIC_TELEMETRY  "restaurant/telemetry"
#define PUB_TOPIC_CONTROL    "restaurant/control"
```

- [ ] **Step 2: Add public broker defines to config.h**

Same block added after `MQTT_PORT`:

```cpp
// 公共 MQTT Broker (用于 Web 控制台实时通信)
#define PUB_BROKER      "broker.emqx.io"
#define PUB_PORT        1883
#define PUB_TOPIC_TELEMETRY  "restaurant/telemetry"
#define PUB_TOPIC_CONTROL    "restaurant/control"
```

- [ ] **Step 3: Commit config changes**

```bash
git add firmware/esp32-iot/config.example.h firmware/esp32-iot/config.h
git commit -m "feat: add public MQTT broker config for web console relay"
```

---

### Task 2: Add second MQTT client + public broker logic to firmware

**Files:**
- Modify: `firmware/esp32-iot/esp32-iot.ino`

- [ ] **Step 1: Add second WiFiClient and PubSubClient declarations**

After `WiFiClient wifiClient;` and `PubSubClient mqtt(wifiClient);` (line ~12-13), add:

```cpp
// 公共 MQTT Broker (Web 控制台中继)
WiFiClient pubWifiClient;
PubSubClient pubMqtt(pubWifiClient);
```

- [ ] **Step 2: Add public broker connection function**

Add this function before `setup()`, after `reconnectMQTT()`:

```cpp
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
```

- [ ] **Step 3: Add public broker control callback**

Add this function before `setup()`, after `reconnectPubMQTT()`:

```cpp
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
```

- [ ] **Step 4: Modify reportTelemetry() to also publish to public broker**

Inside `reportTelemetry()`, after the existing `if (mqtt.publish(...` block (after line ~116), add public broker publish:

```cpp
  // 同时向公共 broker 推送简化格式 (Web 控制台实时数据)
  StaticJsonDocument<128> pubDoc;
  pubDoc["temp"]   = round(temp * 10) / 10.0;
  pubDoc["hum"]    = round(hum);
  pubDoc["led"]    = ledState ? 1 : 0;
  pubDoc["bright"] = ledBrightness;
  String pubPayload;
  serializeJson(pubDoc, pubPayload);
  pubMqtt.publish(PUB_TOPIC_TELEMETRY, pubPayload.c_str());
```

- [ ] **Step 5: Initialize public MQTT client in setup()**

Inside `setup()`, after the existing `reconnectMQTT();` (after line ~187), add:

```cpp
  // 初始化公共 MQTT Broker (Web 控制台中继)
  pubMqtt.setServer(PUB_BROKER, PUB_PORT);
  pubMqtt.setCallback(pubMqttCallback);
  reconnectPubMQTT();
```

- [ ] **Step 6: Add public broker loop handling**

Inside `loop()`, after `mqtt.loop();`, add:

```cpp
  if (!pubMqtt.connected()) {
    reconnectPubMQTT();
  }
  pubMqtt.loop();
```

- [ ] **Step 7: Commit firmware changes**

```bash
git add firmware/esp32-iot/esp32-iot.ino
git commit -m "feat: add public MQTT broker relay for web console real-time comm"
```

---

### Task 3: Rewrite web console to use MQTT WebSocket

**Files:**
- Modify: `web-console/index.html`

- [ ] **Step 1: Replace MQTT.js script from on-demand to preloaded**

Remove the dynamic script loading. Replace the inline `<script>` block at line ~311-347 (the `tryConnectMqttAndSend` function) and all surrounding HTTP polling code.

Replace the Chart.js CDN line (line 7):
```html
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
```

With both Chart.js and MQTT.js preloaded:
```html
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<script src="mqtt.min.js"></script>
```

- [ ] **Step 2: Replace the entire `<script>` block (lines 126-402)**

Replace everything from `// ========== 配置 ==========` to the end of the last `</script>` with:

```javascript
// ========== 配置 ==========
const PUB_BROKER_WS  = 'wss://broker.emqx.io:8083/mqtt';
const TOPIC_TELEMETRY = 'restaurant/telemetry';
const TOPIC_CONTROL   = 'restaurant/control';

const TEMP_ALERT_HIGH = 30;
const HUM_ALERT_HIGH  = 80;

let lastTemp = null, lastHum = null, lastLedState = 0, lastLedBright = 0;

// ========== MQTT 连接 (WebSocket) ==========
const mqttClient = mqtt.connect(PUB_BROKER_WS, {
  clientId: 'web_' + Math.random().toString(16).slice(2, 10),
  keepalive: 60,
  reconnectPeriod: 3000
});

mqttClient.on('connect', function() {
  console.log('MQTT WebSocket connected');
  document.getElementById('status').textContent = '在线';
  document.getElementById('status').className = 'status online';
  mqttClient.subscribe(TOPIC_TELEMETRY);
});

mqttClient.on('message', function(topic, payload) {
  if (topic === TOPIC_TELEMETRY) {
    try {
      const data = JSON.parse(payload.toString());
      updateFromTelemetry(data);
    } catch(e) {
      console.log('Parse error:', e);
    }
  }
});

mqttClient.on('error', function(err) {
  console.log('MQTT error:', err);
  document.getElementById('status').textContent = '离线 (连接失败)';
  document.getElementById('status').className = 'status offline';
});

mqttClient.on('offline', function() {
  document.getElementById('status').textContent = '离线 (重连中...)';
  document.getElementById('status').className = 'status offline';
});

// ========== 解析遥测数据 ==========
function updateFromTelemetry(data) {
  // data: {"temp":25.3,"hum":62,"led":0,"bright":60}
  if (typeof data.temp === 'number' && !isNaN(data.temp)) {
    document.getElementById('tempVal').textContent = data.temp.toFixed(1);
    const card = document.getElementById('tempCard');
    if (data.temp > TEMP_ALERT_HIGH) {
      card.classList.add('warn');
      showToast('⚠️ 温度过高: ' + data.temp.toFixed(1) + '°C');
    } else {
      card.classList.remove('warn');
    }
    lastTemp = data.temp;
  }

  if (typeof data.hum === 'number' && !isNaN(data.hum)) {
    document.getElementById('humVal').textContent = Math.round(data.hum);
    const card = document.getElementById('humCard');
    if (data.hum > HUM_ALERT_HIGH) {
      card.classList.add('warn');
      showToast('⚠️ 湿度过高: ' + Math.round(data.hum) + '%');
    } else {
      card.classList.remove('warn');
    }
    lastHum = data.hum;
  }

  if (typeof data.led === 'number') {
    lastLedState = data.led;
    lastLedBright = typeof data.bright === 'number' ? data.bright : 0;
    updateLedUI(lastLedState === 1, lastLedBright);
  }

  // 追加图表数据
  if (lastTemp !== null && lastHum !== null && !isNaN(lastTemp) && !isNaN(lastHum)) {
    const now = new Date();
    const label = now.getHours().toString().padStart(2,'0') + ':' +
                  now.getMinutes().toString().padStart(2,'0');
    if (tempData.labels.length === 0 || tempData.labels[tempData.labels.length-1] !== label) {
      tempData.labels.push(label);
      tempData.datasets[0].data.push(lastTemp);
      tempData.datasets[1].data.push(lastHum);
      if (tempData.labels.length > 50) {
        tempData.labels.shift();
        tempData.datasets[0].data.shift();
        tempData.datasets[1].data.shift();
      }
      chart.update();
    }
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

function toggleLedManual() {
  const btn = document.getElementById('ledBtn');
  const newState = btn.textContent === 'OFF';
  const slider = document.getElementById('brightness');
  updateLedUI(newState, parseInt(slider.value));
}

// ========== LED 控制 (MQTT WebSocket) ==========
function sendLedCmd() {
  const btn = document.getElementById('ledBtn');
  const slider = document.getElementById('brightness');
  const state = btn.textContent === 'ON' ? 1 : 0;
  const brightness = parseInt(slider.value);

  if (!mqttClient.connected) {
    showToast('MQTT 未连接, 请稍后重试');
    return;
  }

  const msg = JSON.stringify({ led: state, bright: brightness });
  mqttClient.publish(TOPIC_CONTROL, msg);
  showToast('已发送: LED ' + (state ? '开' : '关') + ', 亮度 ' + brightness + '%');
}

// ========== 告警弹窗 ==========
let toastTimer;
function showToast(text) {
  const el = document.getElementById('toast');
  el.textContent = text;
  el.style.display = 'block';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function() { el.style.display = 'none'; }, 5000);
}

// ========== 历史曲线 ==========
const tempData = {
  labels: [],
  datasets: [
    { label: '温度 (°C)', data: [], borderColor: '#e8734a', backgroundColor: 'rgba(232,115,74,.1)', tension: 0.4, fill: true, yAxisID: 'y' },
    { label: '湿度 (%)',  data: [], borderColor: '#42a5f5', backgroundColor: 'rgba(66,165,245,.1)',  tension: 0.4, fill: true, yAxisID: 'y1' }
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
      y: {
        type: 'linear', position: 'left', min: 0, max: 50,
        ticks: { font: { size: 10 }, callback: function(v) { return v + '°C'; } },
        grid: { color: '#f0f0f0' }
      },
      y1: {
        type: 'linear', position: 'right', min: 0, max: 100,
        ticks: { font: { size: 10 }, callback: function(v) { return v + '%'; } },
        grid: { drawOnChartArea: false }
      }
    }
  }
});

// ========== 亮度滑块 ==========
document.getElementById('brightness').addEventListener('input', function() {
  document.getElementById('bval').textContent = this.value + '%';
  updateLedUI(document.getElementById('ledBtn').textContent === 'ON', parseInt(this.value));
});
```

- [ ] **Step 3: Remove unused config constants from HTML**

The old config block (lines 128-136) with `PRODUCT_ID`, `DEVICE_NAME`, `DEVICE_KEY`, `ACCESS_KEY`, `MQTT_TOKEN`, `POLL_INTERVAL` is gone — the new config block replaces it with just the public broker params.

- [ ] **Step 4: Copy updated HTML to docs/ for GitHub Pages deployment**

```bash
cp web-console/index.html docs/index.html && cp web-console/mqtt.min.js docs/mqtt.min.js
```

- [ ] **Step 5: Commit web console changes**

```bash
git add web-console/index.html docs/index.html
git commit -m "feat: switch web console to public MQTT broker (real-time telemetry + LED control)"
```

---

### Task 4: Compile and verify firmware

**Files:**
- `firmware/esp32-iot/esp32-iot.ino`

- [ ] **Step 1: Verify compilation**

Use Arduino CLI or IDE to compile the sketch. Check for:
- No syntax errors from the second MQTT client additions
- `random()` function available (Arduino built-in)
- No duplicate function/variable names

Expected: compilation succeeds with no errors.

- [ ] **Step 2: Flash to ESP32 and verify via serial monitor**

Expected serial output:
```
Pub MQTT connecting...connected
Subscribed: restaurant/control
Reported: xx.x°C, xx%
```

---

### Task 5: Verify web console end-to-end

**Files:**
- `web-console/index.html`

- [ ] **Step 1: Open web console in browser**

Open `web-console/index.html` locally. Expected:
- Status shows "在线" after MQTT WebSocket connects
- Temperature and humidity values appear within 60 seconds
- Chart starts accumulating data points

- [ ] **Step 2: Test LED control**

Click LED button → adjust brightness → click "确认发送". Expected:
- Toast shows "已发送"
- ESP32 serial monitor shows "Pub control received: ..."
- LED on ESP32 responds

- [ ] **Step 3: Deploy to GitHub Pages**

```bash
cd docs && git add index.html mqtt.min.js && git commit -m "deploy: update GitHub Pages with public MQTT relay" && git push
```

Verify at: `https://kevin001-cell.github.io/restaurant-iot/`
