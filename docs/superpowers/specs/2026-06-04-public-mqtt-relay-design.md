# 公共 MQTT Broker 中继 — 设计文档

**日期**: 2026-06-04  
**状态**: 设计中  
**关联**: [OneNET 迁移设计](2026-06-01-onenet-migration-design.md)

## 问题

Web 控制台无法直接与 OneNET 通信：
- OneNET HTTP API (`iot-api.heclouds.com`) 有 CORS 限制，浏览器 fetch 被阻止
- OneNET MQTT WebSocket (`wss://mqtts.heclouds.com:8083/mqtt`) 连接失败

## 架构

```
ESP32S3 ──TCP MQTT (1883)──▶ OneNET (mqtts.heclouds.com)
  │                             └─ 数据存储 + 历史查询
  │
  └──────TCP MQTT (1883)──▶ broker.emqx.io (公共 EMQX)
                               │
Web 控制台 ──WebSocket (8083)──▶ wss://broker.emqx.io:8083/mqtt
                                 └─ 实时数据 + LED 控制
```

**选型理由**: broker.emqx.io 是 EMQX 官方免费公共 MQTT broker，国内可访问，已验证支持浏览器 WebSocket。

## Topic 设计（公共 Broker）

| 方向 | Topic | QoS | 载荷 |
|------|-------|-----|------|
| ESP32 → Web | `restaurant/telemetry` | 0 | `{"temp":25.3,"hum":62,"led":0,"bright":60}` |
| Web → ESP32 | `restaurant/control` | 0 | `{"led":1,"bright":80}` |

- **不设鉴权**（公共 broker，温湿度数据不敏感）
- **QoS 0**（丢一包不影响体验，下一包 60 秒内就到）
- **clientId**: 随机生成，避免冲突

## 固件改动

文件: `firmware/esp32-iot/esp32-iot.ino`

1. **新增第二个 MQTT 客户端** — `WiFiClient pubClient` + `PubSubClient pubMqtt` 连接 `broker.emqx.io:1883`
2. **telemetry 双推** — `reportTelemetry()` 中同时向 OneNET 和公共 broker 各推一份 JSON（公共 broker 用简化格式）
3. **订阅 control topic** — `pubMqtt` 订阅 `restaurant/control`，回调中执行 LED 操作
4. **重连逻辑** — 两个 MQTT 独立 reconnect，互不影响
5. **config.h** — 新增公共 broker 参数（地址、端口、topic）

## Web 控制台改动

文件: `web-console/index.html`

1. **页面加载时预加载 MQTT.js**（不再按需加载）
2. **连接公共 broker** — `mqtt.connect('wss://broker.emqx.io:8083/mqtt')`
3. **订阅 `restaurant/telemetry`** — 实时更新温度/湿度/LED UI + 追加图表数据
4. **发布 `restaurant/control`** — 发送 LED 开关/亮度指令
5. **移除** HTTP 轮询、JSONP、fetch、CORS 相关代码
6. **移除** 动态 script 加载逻辑
7. **状态指示** — 根据 MQTT 连接状态显示在线/离线
8. **曲线** — 前端累积数据（最多 50 个点），页面刷新后清空（可接受）

## 安全考量

- 公共 broker 无鉴权，任何人可订阅/发布 — 可接受（数据仅为餐厅温湿度，不敏感）
- 如后续需要安全性，可升级为 EMQX Cloud 免费试用实例（带 ACL）

## 自检

- [x] 无 TBD/占位符
- [x] 架构与功能描述一致
- [x] 范围适单（固件 + Web 改两个文件）
- [x] 无歧义
