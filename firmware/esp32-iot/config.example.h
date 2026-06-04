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

// 公共 MQTT Broker (用于 Web 控制台实时通信)
#define PUB_BROKER      "broker.emqx.io"
#define PUB_PORT        1883
#define PUB_TOPIC_TELEMETRY  "restaurant/telemetry"
#define PUB_TOPIC_CONTROL    "restaurant/control"

#endif
