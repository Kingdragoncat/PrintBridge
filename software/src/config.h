#pragma once
#ifndef CONFIG_H
#define CONFIG_H

// If config.local.h exists, use private user credentials (git-ignored)
#if __has_include("config.local.h")
#include "config.local.h"
#else

// ---- WiFi Credentials ----
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---- Static IP Configuration (Optional) ----
#define USE_STATIC_IP false
#define STATIC_IP      192, 168, 1, 50
#define STATIC_GATEWAY 192, 168, 1, 1
#define STATIC_SUBNET  255, 255, 255, 0
#define STATIC_DNS     192, 168, 1, 1

#endif

// ---- Network ----
#define PRINT_PORT 9100 // raw TCP / JetDirect

// ---- Hardware Pins (ESP32-S3 custom PCB) ----
// USB host D+/D- are handled internally by the S3's USB PHY (GPIO19/20)

// Status LED (WS2812B)
#define LED_DATA_PIN 4 // Pin IO4 on schematic
#define NUM_LEDS 1

// MicroSD Card (SPI)
#define SD_CS 9    // Pin IO9 on schematic
#define SD_MOSI 10 // Pin IO10 on schematic
#define SD_SCK 11  // Pin IO11 on schematic
#define SD_MISO 12 // Pin IO12 on schematic

// UART is on default U0 pins (TX=43, RX=44)

#endif
