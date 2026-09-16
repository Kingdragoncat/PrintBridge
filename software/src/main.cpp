/*
 * PrintBridge — WiFi print server firmware (WIP)
 *
 * Turns a wired USB printer into a network printer.
 * Receives print jobs over WiFi (raw TCP port 9100),
 * spools them to the SD card so a wifi hiccup doesnt kill the print,
 * then feeds the data to the printer over USB Host.
 *
 * Hardware: ESP32-S3-WROOM-1, USB-A host port, micro SD, WS2812B status LED
 * Flash via UART header with an external USB-serial adapter.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <FastLED.h>
#include <SD.h>
#include <SPI.h>
#include "config.h"

// --- status LED ---
CRGB leds[NUM_LEDS];

// --- network ---
WiFiServer printServer(PRINT_PORT);
WiFiClient client;

// --- state machine ---
enum State {
    BOOTING,
    CONNECTING_WIFI,
    READY,
    RECEIVING,    // downloading print job from wifi -> SD
    PRINTING,     // feeding spooled data from SD -> USB printer
    ERR
};
State state = BOOTING;

// job counter so each print gets a unique spool file
uint32_t jobNumber = 0;

// =========================================================
//  Status LED — colors match what i described in devlog #3
// =========================================================
void setLED(State s) {
    state = s;
    switch (s) {
        case BOOTING:         leds[0] = CRGB::Blue;   break;
        case CONNECTING_WIFI: leds[0] = CRGB::Yellow;  break;
        case READY:           leds[0] = CRGB::Green;   break;
        case RECEIVING:       leds[0] = CRGB::Cyan;    break;
        case PRINTING:        leds[0] = CRGB::Purple;  break;
        case ERR:             leds[0] = CRGB::Red;     break;
    }
    FastLED.show();
}

// =========================================================
//  WiFi
// =========================================================
void connectWiFi() {
    setLED(CONNECTING_WIFI);
    Serial.println("[wifi] connecting...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // try for 10 seconds then give up
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[wifi] failed to connect");
        setLED(ERR);
    }
}

// =========================================================
//  SD card
// =========================================================
bool initSD() {
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        Serial.println("[sd] card init failed — is one inserted?");
        return false;
    }
    Serial.printf("[sd] card ready, %lluMB\n", SD.cardSize() / (1024 * 1024));
    return true;
}

// spool incoming data to a file on the SD card
// returns the filename so we can read it back later
String spoolToSD(WiFiClient &c) {
    jobNumber++;
    String path = "/spool/job_" + String(jobNumber) + ".raw";

    // make sure spool dir exists
    if (!SD.exists("/spool")) SD.mkdir("/spool");

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("[sd] couldnt open spool file");
        return "";
    }

    uint8_t buf[1024];
    uint32_t total = 0;

    // read everything the client sends until they disconnect
    while (c.connected() || c.available()) {
        int n = c.available();
        if (n > 0) {
            int toRead = min(n, (int)sizeof(buf));
            int got = c.read(buf, toRead);
            if (got > 0) {
                f.write(buf, got);
                total += got;
            }
        } else {
            delay(10);  // chill for a sec, more data might be coming
        }
    }

    f.close();
    Serial.printf("[sd] spooled job #%d — %d bytes -> %s\n", jobNumber, total, path.c_str());
    return path;
}

// =========================================================
//  USB Host  (WIP — this is the hard part)
// =========================================================

// TODO: implement actual USB host printer class driver
// the ESP-IDF has a usb_host library that handles enumeration
// and theres an arduino wrapper (EspUsbHost) that might work
// for now this is just a stub that pretends to send data

void initUSBHost() {
    Serial.println("[usb] host init (WIP — not implemented yet)");
    // TODO: call usb_host_install() and set up the printer client
    // see: https://github.com/tanakamasayuki/EspUsbHost
}

// read the spooled file and send it to the printer over USB
bool printFromSD(String path) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.println("[usb] couldnt open spool file for printing");
        return false;
    }

    uint8_t buf[512];
    uint32_t sent = 0;

    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n > 0) {
            // TODO: actually send buf to USB printer here
            // something like: usb_printer_write(buf, n);
            sent += n;
        }
    }

    f.close();
    Serial.printf("[usb] sent %d bytes to printer (WIP — data discarded for now)\n", sent);

    // clean up the spool file after printing
    SD.remove(path);
    return true;
}

// =========================================================
//  Setup
// =========================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    // LED first so we get visual feedback immediately
    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(40);
    setLED(BOOTING);

    Serial.println("\n=== PrintBridge ===");
    Serial.println("WiFi print server — WIP firmware\n");

    // SD card — needed for spooling print jobs
    bool sdOK = initSD();
    if (!sdOK) {
        Serial.println("[!!] no SD card, will try direct streaming (sketchy)");
    }

    // USB host
    initUSBHost();

    // WiFi + start listening for print jobs
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
        printServer.begin();
        Serial.printf("[net] listening on port %d\n", PRINT_PORT);
        setLED(READY);
    }
}

// =========================================================
//  Main loop — accept connections, spool, print
// =========================================================
void loop() {
    // reconnect wifi if it drops
    if (WiFi.status() != WL_CONNECTED && state != ERR) {
        Serial.println("[wifi] connection lost, reconnecting...");
        connectWiFi();
        if (WiFi.status() == WL_CONNECTED) {
            printServer.begin();
            setLED(READY);
        }
        return;
    }

    // check for incoming print jobs
    client = printServer.available();
    if (client) {
        Serial.println("[net] new print job incoming");
        setLED(RECEIVING);

        // step 1: spool the entire job to SD so wifi drops cant kill it
        String spoolPath = spoolToSD(client);
        client.stop();

        if (spoolPath.length() > 0) {
            // step 2: feed the spooled data to the printer
            setLED(PRINTING);
            printFromSD(spoolPath);
        }

        setLED(READY);
        Serial.println("[net] ready for next job");
    }

    delay(10);
}
