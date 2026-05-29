# ESP32S3 温湿度监控 + LED远程控制 — 设计方案

**日期:** 2026-05-29
**状态:** 已确认

## 1. 需求概述

使用 ESP32S3 采集餐厅大堂温湿度数据，通过 MQTT 协议上传至腾讯云 IoT Explorer，并提供独立 Web 控制台实时查看数据、远程控制 LED 灯亮灭与亮度。

## 2. 系统架构

```
┌──────────────────────────────────────────────────────────┐
│                    腾讯云 IoT Explorer                     │
│  设备认证(密钥) │ 规则引擎 │ 时序数据库(30天)             │
└──────┬───────────────┬──────────────────────────────────┘
       │ MQTT/TLS      │ MQTT/WebSocket(TLS)
       ▼               ▼
  ┌──────────┐    ┌──────────────┐
  │ ESP32S3  │    │  Web 控制台   │
  │ SHT30    │    │  MQTT.js     │
  │ PWM LED  │    │  CloudBase   │
  └──────────┘    └──────────────┘
```

### 数据流

1. **上报**: ESP32S3 每60秒采集 SHT30 → publish 到 `$thing/up/property/{device}` → IoT Hub 存储 → Web 通过 MQTT.js subscribe 实时接收
2. **控制**: Web 面板 publish → `$thing/down/property/{device}` → IoT Hub 转发 → ESP32S3 subscribe 接收 → GPIO/PWM 执行
3. **历史**: Web 调用腾讯云 IoT API 查询时序数据库，渲染历史曲线
4. **告警**: Web 端判断阈值（温度>30°C 或湿度>80%），页面弹窗提醒

## 3. 硬件设计

| 组件 | 型号 | 接口 | 说明 |
|------|------|------|------|
| 主控 | ESP32S3 | - | WiFi + BLE 双核 |
| 传感器 | SHT30 | I2C (SDA/SCL) | ±0.3°C, ±2%RH 精度 |
| LED | 普通单色LED | GPIO + PWM | 0-100% 亮度可调 |
| 电源 | USB 5V | Micro-USB/Type-C | 或 3.7V 锂电池 |

### 引脚连接

| ESP32S3 | 外设 |
|---------|------|
| GPIO21 (SDA) | SHT30 SDA |
| GPIO22 (SCL) | SHT30 SCL |
| GPIO5 (PWM) | LED 正极 (串220Ω电阻) |
| GND | 共地 |

## 4. 固件设计 (Arduino/C++)

### 核心库

- `WiFi.h` — WiFi 连接
- `PubSubClient.h` — MQTT 客户端
- `Adafruit_SHT31.h` — SHT30 传感器驱动
- `ArduinoJson.h` — JSON 序列化/反序列化

### 主循环逻辑

```
setup():
  1. 初始化串口、I2C、WiFi
  2. 连接 WiFi (WiFiManager 配网)
  3. 初始化 MQTT (设备密钥认证)
  4. 订阅 $thing/down/property/{device}

loop():
  1. MQTT 保活 (keepalive)
  2. 每60秒: 读取 SHT30 → JSON序列化 → publish 上报
  3. 收到 control 消息: 解析 JSON → PWM 控制 LED
  4. 上报 LED 当前状态回执
```

### MQTT Topic (腾讯云 IoT 标准格式)

| Topic | 方向 | Payload 示例 |
|-------|------|-------------|
| `$thing/up/property/{product_id}/{device_name}` | 上报 | `{"method":"report","clientToken":"xxx","params":{"temperature":25.3,"humidity":62}}` |
| `$thing/down/property/{product_id}/{device_name}` | 下发 | `{"method":"control","clientToken":"xxx","params":{"led_switch":1,"led_brightness":60}}` |

## 5. Web 控制台设计

### 技术方案

- 单 HTML 文件，部署到 CloudBase 静态网站托管
- MQTT.js 库 (CDN) 通过 WebSocket 连接 IoT Hub
- Chart.js (CDN) 渲染历史温湿度曲线
- 腾讯云 IoT API (Node.js SDK 封装为云函数) 查询历史数据

### 界面模块

1. **实时数据卡片**: 当前温度、湿度、LED状态，每收到MQTT消息自动刷新
2. **LED控制面板**: 开关按钮 + 亮度滑块(0-100%) + 确认发送
3. **历史曲线**: 近24小时温湿度折线图 (Chart.js)
4. **告警指示**: 阈值超限时红色闪烁提示

### 部署

- CloudBase 静态网站托管 → 一个 URL 即可访问
- 无需服务器，无需域名备案

## 6. 安全设计

- MQTT 连接使用 TLS 加密
- 设备认证使用腾讯云 IoT 密钥认证 (product_key + device_secret)
- Web 端 MQTT 凭证通过环境变量注入，不硬编码
- LED 控制指令使用 clientToken 防重放

## 7. 开发步骤 (概要)

1. 腾讯云 IoT Explorer 创建产品和设备
2. ESP32S3 固件开发 (传感器采集 + MQTT 上报 + LED控制订阅)
3. Web 控制台开发 (MQTT.js 连接 + UI + 历史曲线)
4. 联调测试
5. 部署上线

## 8. 需求回顾

| 需求项 | 决策 |
|--------|------|
| 云平台 | 腾讯云 IoT Explorer |
| 固件框架 | Arduino (C++) |
| 传感器 | SHT30 (I2C) |
| LED | 单色 PWM 调光 |
| 场景 | 餐厅大堂 |
| 上报频率 | 每60秒 |
| 数据保留 | 30天 |
| 告警 | 温度>30°C / 湿度>80% |
| 控制台 | 独立 Web (CloudBase 托管) |
| 架构 | MQTT 直连模式 |
