#ifndef CONFIG_H
#define CONFIG_H

// ---- WiFi ----
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---- Network ----
#define PRINT_PORT 9100  // raw TCP / JetDirect

// ---- Pins ----
// USB host D+/D- are handled internally by the S3's USB PHY (GPIO19/20)
// so we don't actually need to define them here, the hardware just does its thing

// status LED (WS2812B)
#define LED_DATA_PIN  48  // check schematic — LED_DATA net
#define NUM_LEDS      1

// SD card (SPI)
#define SD_CS   10   // directly from schematic: SD_CS
#define SD_MOSI 11   // SD_MOSI
#define SD_SCK  12   // SD_SCK
#define SD_MISO 13   // SD_MISO

// UART is on the default U0 pins (TX=43, RX=44) so nothing to configure

#endif
