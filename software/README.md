# PrintBridge Firmware (WIP)

firmware for PrintBridge — the ESP32-S3 wifi print server. receives print jobs over wifi, spools them to the SD card, and forwards them to the USB printer.

**status: work in progress.** wifi + SD spooling + TCP server are functional. USB host printer driver is stubbed out (the hard part).

## building

this is a PlatformIO project using the Arduino framework.

1. open this folder in VSCode with PlatformIO installed
2. edit `src/config.h` and put in your wifi credentials
3. hit Build

## flashing

connect an external USB-to-UART adapter to the 4-pin UART header on the board (3V3, GND, TX, RX). hold BOOT, press RESET, release BOOT to enter flash mode. then hit Upload in PlatformIO.

## whats done

- [x] wifi connection (STA mode)
- [x] TCP server on port 9100 (JetDirect)
- [x] SD card spooler — downloads entire print job to SD before printing so wifi drops dont kill it
- [x] WS2812B status LED (blue=boot, yellow=wifi, green=ready, cyan=receiving, purple=printing, red=error)
- [x] wifi auto-reconnect

## whats not done yet

- [ ] USB host initialization (ESP-IDF usb_host library)
- [ ] USB printer class driver (the actual talking-to-printer part)
- [ ] AP mode for initial wifi config (currently hardcoded in config.h)
- [ ] web interface for status/config
- [ ] proper error handling and retry logic
