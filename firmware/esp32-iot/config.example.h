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
