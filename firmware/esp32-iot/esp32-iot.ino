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
WiFiServer  server(80);       // HTTP 服务器

// ========== LED 状态 ==========
bool   ledState = false;
int    ledBrightness = 0;     // 0-100
const int LED_PIN = 5;
volatile bool statusReportPending = false;

// ========== 传感器缓存 (HTTP API 用) ==========
float  lastTemp = 0;
float  lastHum  = 0;
bool   hasReadings = false;

// ========== SHT30 状态 ==========
bool shtAvailable = false;

// ========== 上报计时 ==========
unsigned long lastReportTime = 0;
const unsigned long REPORT_INTERVAL = 60000;  // 60秒

// ========== MQTT Topic 拼接 ==========
const String TOPIC_POST  = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post";
const String TOPIC_SET   = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/set";

// ========== 内嵌 Web 页面 ==========
const char HTML_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"><title>餐厅温湿度</title>
<style>*{margin:0;padding:0;box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:linear-gradient(135deg,#fff3e0,#ffe0b2);min-height:100vh;padding:16px;color:#333}.container{max-width:480px;margin:0 auto}h1{text-align:center;font-size:22px;margin-bottom:4px;color:#e8734a}.subtitle{text-align:center;font-size:13px;color:#999;margin-bottom:16px}.cards{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px}.card{background:#fff;border-radius:16px;padding:20px;text-align:center;box-shadow:0 2px 12px rgba(0,0,0,.08)}.card .value{font-size:48px;font-weight:700;color:#e8734a}.card .unit{font-size:16px;color:#e8734a}.card .label{font-size:13px;color:#999;margin-top:4px}.card.warn .value{color:#e53935}.card.warn{animation:pulse 1.5s ease-in-out infinite}@keyframes pulse{0%,100%{box-shadow:0 2px 12px rgba(0,0,0,.08)}50%{box-shadow:0 0 24px rgba(229,57,53,.3)}}.led-panel{background:#fff;border-radius:16px;padding:20px;margin-bottom:16px;box-shadow:0 2px 12px rgba(0,0,0,.08)}.led-panel h3{font-size:16px;margin-bottom:12px;color:#555}.led-row{display:flex;align-items:center;gap:12px;margin-bottom:12px}.led-dot{width:16px;height:16px;border-radius:50%;background:#ccc;transition:background .3s,box-shadow .3s}.led-dot.on{background:#ff9800;box-shadow:0 0 12px rgba(255,152,0,.6)}.led-btn{flex:1;padding:10px;border:none;border-radius:10px;font-size:16px;font-weight:600;color:#fff;cursor:pointer;transition:all .2s}.led-btn.on{background:#ff9800}.led-btn.off{background:#bdbdbd}.slider-row{display:flex;align-items:center;gap:10px}.slider-row label{font-size:13px;color:#888;white-space:nowrap}.slider-row input[type=range]{flex:1;accent-color:#e8734a}.slider-row span{width:40px;text-align:center;font-weight:600;color:#e8734a}.send-btn{margin-top:12px;width:100%;padding:12px;border:none;border-radius:10px;background:#e8734a;color:#fff;font-size:16px;font-weight:600;cursor:pointer}.send-btn:active{opacity:.8}.chart-panel{background:#fff;border-radius:16px;padding:20px;margin-bottom:16px;box-shadow:0 2px 12px rgba(0,0,0,.08)}.chart-panel h3{font-size:16px;margin-bottom:8px;color:#555}.chart-wrap{position:relative;height:200px}.chart-wrap canvas{width:100%!important}.status{text-align:center;font-size:12px;padding:8px;border-radius:8px;margin-bottom:12px}.status.online{background:#e8f5e9;color:#2e7d32}.status.offline{background:#ffebee;color:#c62828}.toast{position:fixed;top:20px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:12px 24px;border-radius:12px;font-size:14px;font-weight:600;z-index:999;box-shadow:0 4px 20px rgba(0,0,0,.3);animation:slideDown .3s ease-out;display:none}@keyframes slideDown{from{transform:translateX(-50%) translateY(-60px);opacity:0}to{transform:translateX(-50%) translateY(0);opacity:1}}</style></head><body>
<div class="container"><h1>餐厅环境监控</h1><p class="subtitle">屁屁点餐 · 智能温湿度</p>
<div class="status offline" id="status">连接中...</div>
<div class="cards"><div class="card" id="tempCard"><div class="value"><span id="tv">--</span><span class="unit">C</span></div><div class="label">温度</div></div>
<div class="card" id="humCard"><div class="value"><span id="hv">--</span><span class="unit">%</span></div><div class="label">湿度</div></div></div>
<div class="led-panel"><h3>灯光控制</h3>
<div class="led-row"><div class="led-dot off" id="ld"></div><button class="led-btn off" id="lbtn" onclick="tL()">OFF</button></div>
<div class="slider-row"><label>亮度</label><input type="range" min="0" max="100" value="0" id="bri"><span id="bval">0%</span></div>
<button class="send-btn" onclick="sC()">确认发送</button></div>
<div class="chart-panel"><h3>温湿度趋势</h3><div class="chart-wrap"><canvas id="chart"></canvas></div></div></div>
<div class="toast" id="toast"></div>
<script>
var L=0,B=0,on=false,C=null,D=[],T=[],H=[];

// 核心轮询 — 不依赖 Chart.js
function P(){var st=Date.now();fetch('/api/telemetry?t='+st).then(function(r){return r.json()}).then(function(d){
var dt=new Date(),ts=String(dt.getHours()).padStart(2,'0')+':'+String(dt.getMinutes()).padStart(2,'0')+':'+String(dt.getSeconds()).padStart(2,'0');
var s=document.getElementById('status'),elapsed=Date.now()-st;
s.textContent='在线 · 更新 '+ts+' ('+elapsed+'ms)';s.className='status online';
var t=d.temp,h=d.hum;document.getElementById('tv').textContent=t.toFixed(1);
document.getElementById('hv').textContent=Math.round(h);
document.getElementById('tempCard').classList.toggle('warn',t>30);
document.getElementById('humCard').classList.toggle('warn',h>80);
if(!on||d.led!=L||d.bright!=B){L=d.led;B=d.bright;U(L==1,B);}
if(C){var dt=new Date();var lb=String(dt.getHours()).padStart(2,'0')+':'+String(dt.getMinutes()).padStart(2,'0');
if(D.length==0||D[D.length-1]!==lb){D.push(lb);T.push(t);H.push(h);if(D.length>50){D.shift();T.shift();H.shift()}C.update();}}
}).catch(function(e){var s=document.getElementById('status');s.textContent='离线';s.className='status offline';});setTimeout(P,5000);}

function U(s,b){document.getElementById('ld').className='led-dot'+(s?' on':'');document.getElementById('lbtn').textContent=s?'ON':'OFF';document.getElementById('lbtn').className='led-btn'+(s?' on':' off');document.getElementById('bri').value=b;document.getElementById('bval').textContent=b+'%';}
function tL(){U(document.getElementById('lbtn').textContent==='OFF',parseInt(document.getElementById('bri').value));on=true;}
function sC(){var s=document.getElementById('lbtn').textContent==='ON'?1:0;var b=parseInt(document.getElementById('bri').value);
fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({led:s,bright:b})}).then(function(){
on=false;S('已发送: LED '+(s?'开':'关')+', 亮度 '+b+'%');}).catch(function(){S('发送失败');});}
function S(t){var e=document.getElementById('toast');e.textContent=t;e.style.display='block';clearTimeout(window._tt);window._tt=setTimeout(function(){e.style.display='none';},3000);}
document.getElementById('bri').addEventListener('input',function(){on=false;document.getElementById('bval').textContent=this.value+'%';U(document.getElementById('lbtn').textContent==='ON',parseInt(this.value));});

// 启动轮询 (不依赖 Chart.js)
P();

// 尝试加载 Chart.js (非阻塞)
var cs=document.createElement('script');
cs.src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js';
cs.onload=function(){
C=new Chart(document.getElementById('chart').getContext('2d'),{type:'line',data:{labels:D,datasets:[
{label:'温度 C',data:T,borderColor:'#e8734a',backgroundColor:'rgba(232,115,74,.1)',tension:0.4,fill:true,yAxisID:'y'},
{label:'湿度 %',data:H,borderColor:'#42a5f5',backgroundColor:'rgba(66,165,245,.1)',tension:0.4,fill:true,yAxisID:'y1'}]},
options:{responsive:true,maintainAspectRatio:false,plugins:{legend:{position:'bottom',labels:{boxWidth:12,padding:16,font:{size:11}}}},
scales:{x:{display:false},y:{type:'linear',position:'left',min:0,max:50,ticks:{font:{size:10},callback:function(v){return v+' C'}},grid:{color:'#f0f0f0'}},
y1:{type:'linear',position:'right',min:0,max:100,ticks:{font:{size:10},callback:function(v){return v+'%'}},grid:{drawOnChartArea:false}}}}});
};
cs.onerror=function(){document.getElementById('chart').parentElement.innerHTML='<p style=text-align:center;color:#999;padding:40px>Chart.js 加载失败<br>(CDN 不可用)</p>';};
document.head.appendChild(cs);
</script></body></html>
)=====";

// ========== OneNET MQTT 重连 ==========
void reconnectMQTT() {
  while (!mqtt.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, waiting...");
      delay(3000);
      return;
    }
    Serial.print("MQTT connecting...");
    String clientId = DEVICE_NAME;
    String username = PRODUCT_ID;
    String password = MQTT_TOKEN;
    if (mqtt.connect(clientId.c_str(), username.c_str(), password.c_str())) {
      Serial.println("connected");
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

// ========== MQTT 消息回调 ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.concat((char*)payload, length);
  Serial.print("MQTT recv: "); Serial.println(msg);
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
  statusReportPending = true;
}

// ========== 上报 OneNET ==========
void reportTelemetry() {
  if (!shtAvailable) {
    Serial.println("SHT30 not available");
    return;
  }
  float temp, hum;
  if (!sht30.readBoth(&temp, &hum)) {
    Serial.println("SHT30 read error");
    return;
  }
  // 缓存给 HTTP API
  lastTemp = round(temp * 10) / 10.0;
  lastHum  = round(hum);
  hasReadings = true;

  StaticJsonDocument<256> doc;
  char id[16];
  snprintf(id, 16, "%lu", millis());
  doc["id"] = id;
  JsonObject params = doc.createNestedObject("params");
  params.createNestedObject("temperature")["value"]  = lastTemp;
  params.createNestedObject("humidity")["value"]     = lastHum;
  params.createNestedObject("led_switch")["value"]   = ledState ? 1 : 0;
  params.createNestedObject("led_bright")["value"]   = ledBrightness;
  String payload;
  serializeJson(doc, payload);
  if (mqtt.publish(TOPIC_POST.c_str(), payload.c_str())) {
    Serial.printf("Reported: %.1f°C, %.0f%%\n", lastTemp, lastHum);
  } else {
    Serial.println("Publish failed");
  }
}

// ========== 上报 LED 状态 ==========
void reportStatus() {
  StaticJsonDocument<256> doc;
  char id[16];
  snprintf(id, 16, "%lu", millis());
  doc["id"] = id;
  JsonObject params = doc.createNestedObject("params");
  params.createNestedObject("led_switch")["value"]   = ledState ? 1 : 0;
  params.createNestedObject("led_bright")["value"]   = ledBrightness;
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_POST.c_str(), payload.c_str());
  Serial.println("LED status reported");
}

// ========== HTTP 请求处理 ==========
void handleHttpClient() {
  WiFiClient client = server.available();
  if (!client) return;

  // 等待数据到达 (最多 500ms)
  unsigned long t0 = millis();
  while (!client.available() && millis() - t0 < 500) { delay(1); }
  if (!client.available()) { client.stop(); return; }

  String line = client.readStringUntil('\r');
  client.read(); // skip \n

  // ---- GET /api/telemetry ----
  if (line.startsWith("GET /api/telemetry")) {
    // 实时读取传感器 (不依赖60秒上报缓存)
    if (shtAvailable) {
      float t, h;
      if (sht30.readBoth(&t, &h)) {
        lastTemp = round(t * 10) / 10.0;
        lastHum  = round(h);
        hasReadings = true;
      }
    }
    String json = "{\"temp\":" + String(lastTemp, 1) + ",\"hum\":" + String((int)lastHum) +
                  ",\"led\":" + String(ledState ? 1 : 0) + ",\"bright\":" + String(ledBrightness) + "}";
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println();
    client.println(json);
    delay(5);
    client.stop();
    return;
  }

  // ---- POST /api/control ----
  if (line.startsWith("POST /api/control")) {
    // 读取 headers 找到 Content-Length
    int contentLength = 0;
    while (client.available()) {
      String hdr = client.readStringUntil('\r');
      client.read(); // skip \n
      if (hdr.length() == 0 || hdr == "\n") break;  // 空行 = headers 结束
      if (hdr.startsWith("Content-Length:")) {
        contentLength = hdr.substring(15).toInt();
      }
    }
    // 读取 body
    String body = "";
    if (contentLength > 0 && contentLength < 256) {
      unsigned long t1 = millis();
      while (body.length() < (unsigned)contentLength && millis() - t1 < 1000) {
        if (client.available()) {
          body += (char)client.read();
        } else {
          delay(1);
        }
      }
    }
    // 解析 JSON
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      if (doc.containsKey("led")) {
        ledState = doc["led"].as<int>() == 1;
      }
      if (doc.containsKey("bright")) {
        ledBrightness = doc["bright"].as<int>();
      }
      int pwmValue = ledState ? map(ledBrightness, 0, 100, 0, 255) : 0;
      analogWrite(LED_PIN, pwmValue);
      Serial.printf("HTTP LED: %s, brightness: %d%%\n", ledState ? "ON" : "OFF", ledBrightness);
      statusReportPending = true;
    }
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println();
    client.println("{\"ok\":true}");
    delay(5);
    client.stop();
    return;
  }

  // ---- OPTIONS (CORS preflight) ----
  if (line.startsWith("OPTIONS")) {
    client.println("HTTP/1.1 204 No Content");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
    client.println("Access-Control-Allow-Headers: Content-Type");
    client.println("Connection: close");
    client.println();
    delay(5);
    client.stop();
    return;
  }

  // ---- 默认: 返回 Web 页面 ----
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.print(HTML_PAGE);
  delay(5);
  client.stop();
}

// ========== Arduino Setup ==========
void setup() {
  Serial.begin(115200);
  int waitSerial = 0;
  while (!Serial && waitSerial < 50) { delay(100); waitSerial++; }
  Serial.println("\n=== ESP32S3 IoT ===");

  pinMode(LED_PIN, OUTPUT);
  analogWrite(LED_PIN, 0);

  // SHT30
  Wire.begin(15, 16);
  bool shtOk = false;
  int shtRetries = 0;
  while (shtRetries < 3) {
    if (sht30.begin(0x44)) { shtOk = true; break; }
    Serial.println("SHT30 retry...");
    delay(1000);
    shtRetries++;
  }
  shtAvailable = shtOk;
  Serial.println(shtOk ? "SHT30 OK" : "SHT30 failed!");

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  int wifiRetries = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetries < 40) {
    Serial.print("."); delay(500); wifiRetries++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed, restart...");
    ESP.restart();
  }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // OneNET MQTT
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  reconnectMQTT();

  // HTTP Server
  server.begin();
  Serial.println("HTTP server :80");

  // 立即读取一次传感器，避免前端等60秒才有数据
  if (shtAvailable) {
    float t, h;
    if (sht30.readBoth(&t, &h)) {
      lastTemp = round(t * 10) / 10.0;
      lastHum  = round(h);
      hasReadings = true;
      Serial.printf("Init reading: %.1f°C, %.0f%%\n", lastTemp, lastHum);
    }
  }
}

// ========== Arduino Loop ==========
void loop() {
  if (!mqtt.connected()) {
    reconnectMQTT();
  }
  mqtt.loop();

  if (statusReportPending) {
    statusReportPending = false;
    reportStatus();
  }

  unsigned long now = millis();
  if (now - lastReportTime >= REPORT_INTERVAL) {
    lastReportTime = now;
    reportTelemetry();
  }

  handleHttpClient();
}
