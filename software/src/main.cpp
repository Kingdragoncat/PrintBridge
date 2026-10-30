/*
 * PrintBridge — Apple Settings-style Dashboard & Print Server
 *
 * Board: ESP32-S3-WROOM-1 Custom PCB
 * Hardware:
 *   - WS2812B RGB Status LED (GPIO4)
 *   - MicroSD Card Slot via SPI (CS=9, MOSI=10, SCK=11, MISO=12)
 *   - USB-A Host Port (GPIO19 D-, GPIO20 D+)
 *   - Port 9100: Raw JetDirect TCP Print Server
 *   - Port 9101: Web Dashboard & REST API
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <FastLED.h>
#include <SD.h>
#include <SPI.h>
#include "EspUsbHost.h"
#include "PrinterDevice.hpp"
#include "config.h"

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// Override ESP-IDF's brownout init to prevent spurious BOD reset loops during startup
extern "C" void esp_brownout_init(void) {
    // Intentionally left blank to disable hardware BOD
}

// --- Status LED ---
CRGB leds[NUM_LEDS];
CRGB activeColor = CRGB::Green;

// --- Network Servers ---
WiFiServer printServer(PRINT_PORT);
WebServer webServer(9101);
WiFiClient client;

// --- Dedicated SPI for SD Card ---
SPIClass sdSPI(FSPI);

// --- USB Host & Printer Device ---
EspUsbHost usbHost;
printer::PrinterDevice printerDev(usbHost);
bool printerConnected = false;
String printerModelStr = "Ready";
bool paperEmpty = false;
bool printerSelected = true;
bool printerError = false;

// --- Scheduled Print Jobs ---
struct ScheduledPrint {
    String filename;
    uint32_t executeAtSeconds;
    bool active;
};
#define MAX_SCHEDULED 8
ScheduledPrint scheduledJobs[MAX_SCHEDULED];

// --- Forward Declarations ---
void printFromSD(String path);
String spoolJobToSD(WiFiClient &c);

// --- State Variables ---
uint32_t jobNumber = 0;
uint32_t totalBytesReceived = 0;
bool sdMounted = false;
uint64_t sdTotalMB = 0;
uint64_t sdUsedMB = 0;
String currentStatusStr = "Ready";
uint32_t lastCompletedJob = 0;

void setStatusColor(CRGB color) {
    activeColor = color;
    leds[0] = color;
    FastLED.show();
}

bool initSDCard() {
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    pinMode(SD_MISO, INPUT_PULLUP);
    pinMode(SD_MOSI, OUTPUT);
    pinMode(SD_SCK, OUTPUT);

    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (SD.begin(SD_CS, sdSPI, 4000000)) {
        sdMounted = true;
        sdTotalMB = SD.totalBytes() / (1024 * 1024);
        sdUsedMB = SD.usedBytes() / (1024 * 1024);
        if (!SD.exists("/spool")) SD.mkdir("/spool");
        Serial.printf("[SD] Mounted successfully! Capacity: %llu MB, Used: %llu MB\n", sdTotalMB, sdUsedMB);
        return true;
    }
    sdMounted = false;
    sdTotalMB = 0;
    sdUsedMB = 0;
    Serial.println("[SD] No card found on boot or mount failed.");
    return false;
}

// -------------------------------------------------------------
// REST API: /api/status (JSON)
// -------------------------------------------------------------
void handleApiStatus() {
    uint32_t uptime = millis() / 1000;
    uint32_t freeHeap = ESP.getFreeHeap();
    int rssi = WiFi.RSSI();

    String json = "{";
    json += "\"status\":\"" + currentStatusStr + "\",";
    json += "\"uptime\":" + String(uptime) + ",";
    json += "\"freeHeap\":" + String(freeHeap) + ",";
    json += "\"rssi\":" + String(rssi) + ",";
    json += "\"ssid\":\"" + String(WiFi.SSID()) + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"sdMounted\":" + String(sdMounted ? "true" : "false") + ",";
    json += "\"sdTotalMB\":" + String(sdTotalMB) + ",";
    json += "\"sdUsedMB\":" + String(sdUsedMB) + ",";
    json += "\"printerConnected\":" + String(printerConnected ? "true" : "false") + ",";
    json += "\"printerModel\":\"" + printerModelStr + "\",";
    json += "\"paperEmpty\":" + String(paperEmpty ? "true" : "false") + ",";
    json += "\"printerSelected\":" + String(printerSelected ? "true" : "false") + ",";
    json += "\"printerError\":" + String(printerError ? "true" : "false") + ",";
    json += "\"jobNumber\":" + String(jobNumber) + ",";
    json += "\"lastCompletedJob\":" + String(lastCompletedJob) + ",";
    json += "\"totalBytes\":" + String(totalBytesReceived) + ",";
    char hexColor[8];
    snprintf(hexColor, sizeof(hexColor), "#%02X%02X%02X", activeColor.r, activeColor.g, activeColor.b);
    json += "\"ledColor\":\"" + String(hexColor) + "\"";
    json += "}";

    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", json);
}

// -------------------------------------------------------------
// REST API: /api/queue (JSON)
// -------------------------------------------------------------
void handleApiQueue() {
    String json = "{\"files\":[";
    if (sdMounted) {
        File dir = SD.open("/spool");
        if (dir) {
            File entry = dir.openNextFile();
            bool first = true;
            while (entry) {
                if (!entry.isDirectory()) {
                    if (!first) json += ",";
                    first = false;
                    json += "{";
                    json += "\"name\":\"" + String(entry.name()) + "\",";
                    json += "\"size\":" + String(entry.size());
                    json += "}";
                }
                entry = dir.openNextFile();
            }
            dir.close();
        }
    }
    json += "],\"scheduled\":[";
    uint32_t nowSec = millis() / 1000;
    bool firstS = true;
    for (int i = 0; i < MAX_SCHEDULED; i++) {
        if (scheduledJobs[i].active) {
            if (!firstS) json += ",";
            firstS = false;
            int remaining = (int)scheduledJobs[i].executeAtSeconds - (int)nowSec;
            if (remaining < 0) remaining = 0;
            json += "{";
            json += "\"id\":" + String(i) + ",";
            json += "\"name\":\"" + scheduledJobs[i].filename + "\",";
            json += "\"remainingSec\":" + String(remaining);
            json += "}";
        }
    }
    json += "]}";
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", json);
}

// -------------------------------------------------------------
// REST API: /api/schedule
// -------------------------------------------------------------
void handleApiSchedule() {
    if (webServer.hasArg("name") && webServer.hasArg("delay_sec")) {
        String name = webServer.arg("name");
        uint32_t delaySec = webServer.arg("delay_sec").toInt();
        uint32_t nowSec = millis() / 1000;
        for (int i = 0; i < MAX_SCHEDULED; i++) {
            if (!scheduledJobs[i].active) {
                scheduledJobs[i].filename = name;
                scheduledJobs[i].executeAtSeconds = nowSec + delaySec;
                scheduledJobs[i].active = true;
                Serial.printf("[Schedule] Job %s queued in %lu seconds\n", name.c_str(), (unsigned long)delaySec);
                break;
            }
        }
    }
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "{\"scheduled\":true}");
}

// -------------------------------------------------------------
// REST API: /api/cancel_schedule
// -------------------------------------------------------------
void handleApiCancelSchedule() {
    if (webServer.hasArg("id")) {
        int id = webServer.arg("id").toInt();
        if (id >= 0 && id < MAX_SCHEDULED) {
            scheduledJobs[id].active = false;
        }
    }
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "{\"cancelled\":true}");
}

// -------------------------------------------------------------
// REST API: /api/copy (Photocopy trigger)
// -------------------------------------------------------------
void handleApiCopy() {
    int copies = 1;
    if (webServer.hasArg("copies")) copies = webServer.arg("copies").toInt();
    if (copies < 1) copies = 1;
    if (copies > 99) copies = 99;

    if (printerConnected) {
        String pjl = "\033%-12345X@PJL\r\n@PJL SET COPIES=" + String(copies) + "\r\n@PJL ENTER LANGUAGE=PCL\r\n\033E\033%-12345X";
        printerDev.write((const uint8_t*)pjl.c_str(), pjl.length());
        Serial.printf("[MFP] Sent Copy Command for %d copies.\n", copies);
    }
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "{\"copies\":" + String(copies) + "}");
}

// -------------------------------------------------------------
// REST API: /api/clear_queue
// -------------------------------------------------------------
void handleApiClearQueue() {
    if (sdMounted) {
        File dir = SD.open("/spool");
        if (dir) {
            File entry = dir.openNextFile();
            while (entry) {
                String fullPath = "/spool/" + String(entry.name());
                entry.close();
                SD.remove(fullPath);
                entry = dir.openNextFile();
            }
            dir.close();
        }
    }
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "{\"cleared\":true}");
}

// -------------------------------------------------------------
// REST API: /api/delete_job
// -------------------------------------------------------------
void handleApiDeleteJob() {
    if (webServer.hasArg("name")) {
        String name = webServer.arg("name");
        if (name.indexOf("/") == -1 && name.indexOf("\\") == -1) {
            String path = "/spool/" + name;
            if (sdMounted && SD.exists(path)) {
                SD.remove(path);
                Serial.printf("[SD] Deleted spool job: %s\n", path.c_str());
            }
        }
    }
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "{\"deleted\":true}");
}

// -------------------------------------------------------------
// REST API: /api/reprint
// -------------------------------------------------------------
void handleApiReprint() {
    if (webServer.hasArg("name")) {
        String name = webServer.arg("name");
        if (name.indexOf("/") == -1 && name.indexOf("\\") == -1) {
            String path = "/spool/" + name;
            if (sdMounted && SD.exists(path)) {
                printFromSD(path);
            }
        }
    }
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", "{\"reprinted\":true}");
}

// -------------------------------------------------------------
// REST API: /api/led
// -------------------------------------------------------------
void handleApiLED() {
    if (webServer.hasArg("hex")) {
        String hex = webServer.arg("hex");
        if (hex.startsWith("#")) hex = hex.substring(1);
        long num = strtol(hex.c_str(), NULL, 16);
        uint8_t r = (num >> 16) & 0xFF;
        uint8_t g = (num >> 8) & 0xFF;
        uint8_t b = num & 0xFF;
        setStatusColor(CRGB(r, g, b));
    } else if (webServer.hasArg("preset")) {
        String p = webServer.arg("preset");
        if (p == "blue") setStatusColor(CRGB::Blue);
        else if (p == "yellow") setStatusColor(CRGB::Yellow);
        else if (p == "green") setStatusColor(CRGB::Green);
        else if (p == "cyan") setStatusColor(CRGB::Cyan);
        else if (p == "purple") setStatusColor(CRGB::Purple);
        else if (p == "red") setStatusColor(CRGB::Red);
        else if (p == "off") setStatusColor(CRGB::Black);
    }
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "text/plain", "OK");
}

// -------------------------------------------------------------
// Clean Apple System Settings Dashboard
// -------------------------------------------------------------
void handleRoot() {
    String html = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>PrintBridge</title>
<style>
  * { box-sizing: border-box; -webkit-font-smoothing: antialiased; margin: 0; padding: 0; }
  body {
    background-color: #f2f2f7;
    font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Helvetica Neue", sans-serif;
    color: #1c1c1e;
    padding: 40px 16px 60px;
    display: flex;
    justify-content: center;
    user-select: none;
    -webkit-user-select: none;
  }
  .wrapper { width: 100%; max-width: 580px; }
  
  .page-title {
    font-size: 28px;
    font-weight: 700;
    letter-spacing: -0.5px;
    margin-bottom: 24px;
    padding-left: 4px;
  }

  .section-label {
    font-size: 13px;
    font-weight: 400;
    color: #6e6e73;
    text-transform: uppercase;
    letter-spacing: -0.1px;
    margin: 0 0 7px 14px;
  }

  .group {
    background: #ffffff;
    border-radius: 12px;
    margin-bottom: 26px;
    overflow: hidden;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.04);
  }

  .row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 13px 16px;
    min-height: 48px;
    font-size: 15px;
    border-bottom: 0.5px solid #e5e5ea;
  }
  .row:last-child { border-bottom: none; }
  
  .row-label { font-weight: 400; color: #1c1c1e; }
  .row-value {
    color: #8e8e93;
    font-weight: 400;
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .status-pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 14px;
    font-weight: 500;
  }
  .status-pill.mounted { color: #34c759; }
  .status-pill.unmounted { color: #ff3b30; }

  .dot-green { width: 8px; height: 8px; border-radius: 50%; background: #34c759; }
  .dot-red { width: 8px; height: 8px; border-radius: 50%; background: #ff3b30; }

  /* Apple Style Modal Dialog */
  .modal-backdrop {
    position: fixed;
    top: 0; left: 0; right: 0; bottom: 0;
    background: rgba(0, 0, 0, 0.4);
    backdrop-filter: blur(8px);
    -webkit-backdrop-filter: blur(8px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 1000;
  }
  .modal-box {
    background: #ffffff;
    width: 290px;
    border-radius: 14px;
    padding: 20px 18px 16px;
    text-align: center;
    box-shadow: 0 16px 32px rgba(0, 0, 0, 0.18);
    animation: popIn 0.15s cubic-bezier(0.2, 0.9, 0.42, 1);
  }
  @keyframes popIn {
    from { transform: scale(0.92); opacity: 0; }
    to { transform: scale(1); opacity: 1; }
  }
  .modal-title {
    font-size: 17px;
    font-weight: 600;
    color: #000;
    margin-bottom: 6px;
  }
  .modal-msg {
    font-size: 13px;
    color: #666;
    line-height: 1.35;
    margin-bottom: 18px;
    word-break: break-all;
  }
  .modal-actions {
    display: flex;
    gap: 8px;
  }
  .modal-btn {
    flex: 1;
    padding: 10px;
    border-radius: 9px;
    font-size: 14px;
    font-weight: 600;
    border: none;
    cursor: pointer;
    transition: opacity 0.1s;
  }
  .modal-btn:active { opacity: 0.7; }
  .modal-cancel { background: #e5e5ea; color: #000; }
  .modal-primary { background: #0071e3; color: #fff; }
  .modal-danger { background: #ff3b30; color: #fff; }

  /* Queue Action Buttons */
  .del-btn {
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: #ff3b30;
    color: #fff;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    font-size: 13px;
    font-weight: 700;
    cursor: pointer;
    line-height: 1;
    user-select: none;
    transition: transform 0.1s ease;
  }
  .del-btn:active { transform: scale(0.85); }
  .reprint-btn {
    color: #0071e3;
    cursor: pointer;
    font-size: 13px;
    font-weight: 500;
    margin-left: 8px;
    user-select: none;
  }
  .reprint-btn:hover { text-decoration: underline; }

  /* Color picker button without typing cursor */
  .color-button-wrapper {
    width: 28px;
    height: 28px;
    border-radius: 50%;
    cursor: pointer;
    box-shadow: 0 1px 3px rgba(0,0,0,0.15);
    border: 2px solid #ffffff;
    user-select: none;
    -webkit-user-select: none;
    transition: transform 0.1s ease;
  }
  .color-button-wrapper:active {
    transform: scale(0.92);
  }

  .swatch-grid {
    display: flex;
    gap: 10px;
    padding: 14px 16px;
  }
  .swatch {
    width: 26px;
    height: 26px;
    border-radius: 50%;
    border: 1px solid rgba(0,0,0,0.1);
    cursor: pointer;
    transition: transform 0.1s ease;
  }
  .swatch:active { transform: scale(0.9); }
  .swatch-off {
    background: #e5e5ea;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 10px;
    color: #8e8e93;
    font-weight: 600;
  }

  .indicator-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    display: inline-block;
  }
</style>
</head>
<body>

<div class="wrapper">
  <h1 class="page-title">PrintBridge</h1>

  <!-- General -->
  <div class="section-label">General</div>
  <div class="group">
    <div class="row">
      <span class="row-label">Print Server</span>
      <span class="row-value"><span class="status-pill mounted"><span class="dot-green"></span>Active (Port 9100)</span></span>
    </div>
    <div class="row">
      <span class="row-label">Network</span>
      <span class="row-value" id="val-ssid">--</span>
    </div>
    <div class="row">
      <span class="row-label">IP Address</span>
      <span class="row-value" id="val-ip">--</span>
    </div>
    <div class="row">
      <span class="row-label">Signal</span>
      <span class="row-value" id="val-rssi">--</span>
    </div>
    <div class="row">
      <span class="row-label">Uptime</span>
      <span class="row-value" id="val-uptime">--</span>
    </div>
    <div class="row">
      <span class="row-label">Available Memory</span>
      <span class="row-value" id="val-heap">--</span>
    </div>
  </div>

  <!-- Storage -->
  <div class="section-label">Storage</div>
  <div class="group">
    <div class="row">
      <span class="row-label">MicroSD Buffer</span>
      <span class="row-value" id="val-sd-status">
        <span class="status-pill mounted"><span class="dot-green"></span>Mounted</span>
      </span>
    </div>
    <div class="row">
      <span class="row-label">Buffer Capacity</span>
      <span class="row-value" id="val-sd-cap">--</span>
    </div>
    <div class="row">
      <span class="row-label">Jobs Spooled</span>
      <span class="row-value" id="val-jobs">#0</span>
    </div>
  </div>

  <!-- USB Printer -->
  <div class="section-label">USB Printer</div>
  <div class="group">
    <div class="row">
      <span class="row-label">Connection</span>
      <span class="row-value" id="val-printer-status">
        <span class="status-pill unmounted"><span class="dot-red"></span>Disconnected</span>
      </span>
    </div>
    <div class="row">
      <span class="row-label">Printer Model</span>
      <span class="row-value" id="val-printer-model">--</span>
    </div>
    <div class="row">
      <span class="row-label">Paper Tray</span>
      <span class="row-value" id="val-paper-status">
        <span class="status-pill mounted"><span class="dot-green"></span>Loaded</span>
      </span>
    </div>
  </div>

  <!-- Photocopy & MFP Tools -->
  <div class="section-label">Photocopy & Scanner</div>
  <div class="group">
    <div class="row">
      <span class="row-label">Number of Copies</span>
      <span class="row-value" style="gap:12px;">
        <button class="modal-btn" style="width:28px; height:28px; padding:0; background:#e5e5ea; border-radius:50%;" onclick="changeCopies(-1)">-</button>
        <span id="copy-count" style="font-size:16px; font-weight:600; color:#000; min-width:20px; text-align:center;">1</span>
        <button class="modal-btn" style="width:28px; height:28px; padding:0; background:#e5e5ea; border-radius:50%;" onclick="changeCopies(1)">+</button>
      </span>
    </div>
    <div class="row" style="cursor:pointer;" onclick="triggerCopy()">
      <span class="row-label" style="color:#0071e3; font-weight:500;">Start Quick Copy</span>
      <span class="row-value" style="color:#0071e3; font-size:13px; font-weight:500;">Run Job &rsaquo;</span>
    </div>
  </div>

  <!-- Connect Devices -->
  <div class="section-label">Connect Devices</div>
  <div class="group">
    <div class="row" style="cursor:pointer;" onclick="location.href='/api/install.cmd'">
      <span class="row-label" style="color:#0071e3; font-weight:500;">Add to Windows (1-Click)</span>
      <span class="row-value" style="color:#0071e3; font-size:13px; font-weight:500;">Download Installer &rsaquo;</span>
    </div>
    <div class="row">
      <span class="row-label">Apple (macOS / iOS)</span>
      <span class="row-value" style="color:#8e8e93; font-size:13px;">Auto-detected via Bonjour</span>
    </div>
  </div>

  <!-- Print Queue & Spool -->
  <div class="section-label">Print Queue & Spool</div>
  <div class="group" id="queue-group">
    <div class="row" style="cursor:pointer;" onclick="document.getElementById('file-upload-input').click()">
      <span class="row-label" style="color:#0071e3; font-weight:500;">Upload File to Print</span>
      <span class="row-value" style="color:#0071e3; font-size:13px; font-weight:500;">Select Document &rsaquo;</span>
    </div>
    <input type="file" id="file-upload-input" style="display:none;" onchange="onFileSelected(this)">
    <div class="row">
      <span class="row-label">Stored Spool Jobs</span>
      <span class="row-value" id="val-queue-count">0 files</span>
    </div>
    <div id="queue-items"></div>
    <div id="scheduled-items"></div>
    <div class="row" style="cursor:pointer;" onclick="clearQueue()">
      <span class="row-label" style="color:#ff3b30; font-weight:500;">Clear Spool Files</span>
      <span class="row-value" style="color:#8e8e93; font-size:13px;">Free Space &rsaquo;</span>
    </div>
  </div>

  <!-- Status Light -->
  <div class="section-label">Status Light</div>
  <div class="group">
    <div class="row">
      <span class="row-label">Color Selection</span>
      <span class="row-value">
        <div class="color-button-wrapper" id="color-preview" style="background:#34c759;" onclick="document.getElementById('picker').click()"></div>
        <input type="color" id="picker" value="#34c759" style="position:fixed; top:-1000px; left:-1000px; opacity:0; width:1px; height:1px; pointer-events:none;" oninput="sendColor(this.value)">
      </span>
    </div>
    <div class="swatch-grid">
      <div class="swatch" style="background:#34c759;" onclick="sendPreset('green')"></div>
      <div class="swatch" style="background:#32ade6;" onclick="sendPreset('cyan')"></div>
      <div class="swatch" style="background:#af52de;" onclick="sendPreset('purple')"></div>
      <div class="swatch" style="background:#0071e3;" onclick="sendPreset('blue')"></div>
      <div class="swatch" style="background:#ff9500;" onclick="sendPreset('yellow')"></div>
      <div class="swatch" style="background:#ff3b30;" onclick="sendPreset('red')"></div>
      <div class="swatch swatch-off" onclick="sendPreset('off')">OFF</div>
    </div>
  </div>

  <!-- Legend -->
  <div class="section-label">Color Guide</div>
  <div class="group">
    <div class="row">
      <span class="row-label"><span class="indicator-dot" style="background:#0071e3; margin-right:8px;"></span>Blue</span>
      <span class="row-value">Starting up</span>
    </div>
    <div class="row">
      <span class="row-label"><span class="indicator-dot" style="background:#ff9500; margin-right:8px;"></span>Yellow</span>
      <span class="row-value">Connecting to network</span>
    </div>
    <div class="row">
      <span class="row-label"><span class="indicator-dot" style="background:#34c759; margin-right:8px;"></span>Green</span>
      <span class="row-value">Ready for print jobs</span>
    </div>
    <div class="row">
      <span class="row-label"><span class="indicator-dot" style="background:#32ade6; margin-right:8px;"></span>Cyan</span>
      <span class="row-value">Spooling to MicroSD</span>
    </div>
    <div class="row">
      <span class="row-label"><span class="indicator-dot" style="background:#af52de; margin-right:8px;"></span>Purple</span>
      <span class="row-value">Sending to USB printer</span>
    </div>
    <div class="row">
      <span class="row-label"><span class="indicator-dot" style="background:#ff3b30; margin-right:8px;"></span>Red</span>
      <span class="row-value">Error / Disconnected</span>
    </div>
  </div>

  <!-- Apple Style Custom Modal Dialog -->
  <div id="custom-modal" class="modal-backdrop" style="display:none;" onclick="if(event.target===this)closeModal()">
    <div class="modal-box">
      <div class="modal-title" id="modal-title">Confirm</div>
      <div class="modal-msg" id="modal-msg">Are you sure?</div>
      <div id="modal-extra" style="margin-bottom:14px; display:none;"></div>
      <div class="modal-actions">
        <button class="modal-btn modal-cancel" onclick="closeModal()">Cancel</button>
        <button class="modal-btn modal-primary" id="modal-action-btn" onclick="executeModalAction()">Confirm</button>
      </div>
    </div>
  </div>

</div>

<script>
  let lastNotifiedJob = 0;
  let copyCount = 1;
  let modalCallback = null;

  if ("Notification" in window && Notification.permission !== "granted") {
    Notification.requestPermission();
  }

  function triggerNotification(jobNum, bytes) {
    if ("Notification" in window && Notification.permission === "granted") {
      new Notification("PrintBridge", {
        body: "Print Job #" + jobNum + " completed (" + Math.round(bytes/1024) + " KB)!",
        icon: "https://raw.githubusercontent.com/Kingdragoncat/PrintBridge/main/hardware/images/front.png"
      });
    }
  }

  function showModal({ title, msg, confirmText, isDanger, extraHtml, onConfirm }) {
    document.getElementById('modal-title').innerText = title;
    document.getElementById('modal-msg').innerText = msg;
    const btn = document.getElementById('modal-action-btn');
    btn.innerText = confirmText;
    btn.className = 'modal-btn ' + (isDanger ? 'modal-danger' : 'modal-primary');
    
    const extraEl = document.getElementById('modal-extra');
    if (extraHtml) {
      extraEl.innerHTML = extraHtml;
      extraEl.style.display = 'block';
    } else {
      extraEl.style.display = 'none';
      extraEl.innerHTML = '';
    }
    
    modalCallback = onConfirm;
    document.getElementById('custom-modal').style.display = 'flex';
  }

  function closeModal() {
    document.getElementById('custom-modal').style.display = 'none';
    modalCallback = null;
  }

  function executeModalAction() {
    if (modalCallback) modalCallback();
    closeModal();
  }

  function changeCopies(delta) {
    copyCount = Math.max(1, Math.min(99, copyCount + delta));
    document.getElementById('copy-count').innerText = copyCount;
  }

  function triggerCopy() {
    showModal({
      title: "Start Photocopy",
      msg: `Copy ${copyCount} page${copyCount > 1 ? 's' : ''} on the HP LaserJet? Make sure the document is placed on the scanner glass or ADF.`,
      confirmText: "Start Copy",
      isDanger: false,
      onConfirm: () => {
        fetch('/api/copy?copies=' + copyCount, { method: 'POST' })
          .then(() => refresh());
      }
    });
  }

  function onFileSelected(input) {
    if (!input.files || input.files.length === 0) return;
    const file = input.files[0];
    const kb = Math.round(file.size / 1024);
    
    showModal({
      title: "Print Document",
      msg: `Ready to print "${file.name}" (${kb} KB). Would you like to print now or schedule for later?`,
      confirmText: "Print Now",
      isDanger: false,
      extraHtml: `
        <div style="margin-top:8px;">
          <select id="modal-sched-select" style="width:100%; padding:8px; border-radius:8px; border:1px solid #d1d1d6; font-size:14px; background:#f9f9fb;">
            <option value="0">Print Immediately</option>
            <option value="300">Schedule: in 5 minutes</option>
            <option value="900">Schedule: in 15 minutes</option>
            <option value="1800">Schedule: in 30 minutes</option>
            <option value="3600">Schedule: in 1 hour</option>
          </select>
        </div>
      `,
      onConfirm: () => {
        const schedSec = parseInt(document.getElementById('modal-sched-select').value);
        const formData = new FormData();
        formData.append("file", file);
        fetch('/api/upload_print?delay_sec=' + schedSec, { method: 'POST', body: formData })
          .then(() => {
            loadQueue();
            refresh();
          });
      }
    });
    input.value = "";
  }

  function confirmDeleteJob(name) {
    showModal({
      title: "Delete Spool File?",
      msg: `Permanently remove "${name}" from MicroSD storage?`,
      confirmText: "Delete",
      isDanger: true,
      onConfirm: () => {
        fetch('/api/delete_job?name=' + encodeURIComponent(name), { method: 'POST' })
          .then(() => {
            loadQueue();
            refresh();
          });
      }
    });
  }

  function promptSchedule(name) {
    showModal({
      title: "Schedule Print",
      msg: `When would you like to print "${name}"?`,
      confirmText: "Set Schedule",
      isDanger: false,
      extraHtml: `
        <select id="modal-sched-delay" style="width:100%; padding:8px; border-radius:8px; border:1px solid #d1d1d6; font-size:14px; background:#f9f9fb;">
          <option value="300">In 5 minutes</option>
          <option value="900">In 15 minutes</option>
          <option value="1800">In 30 minutes</option>
          <option value="3600">In 1 hour</option>
          <option value="7200">In 2 hours</option>
        </select>
      `,
      onConfirm: () => {
        const sec = document.getElementById('modal-sched-delay').value;
        fetch('/api/schedule?name=' + encodeURIComponent(name) + '&delay_sec=' + sec, { method: 'POST' })
          .then(() => {
            loadQueue();
            refresh();
          });
      }
    });
  }

  function cancelSchedule(id) {
    fetch('/api/cancel_schedule?id=' + id, { method: 'POST' })
      .then(() => loadQueue());
  }

  function reprintJob(name) {
    showModal({
      title: "Re-Print Job",
      msg: `Send "${name}" to the HP LaserJet right now?`,
      confirmText: "Print Now",
      isDanger: false,
      onConfirm: () => {
        fetch('/api/reprint?name=' + encodeURIComponent(name), { method: 'POST' })
          .then(() => refresh());
      }
    });
  }

  function clearQueue() {
    showModal({
      title: "Clear Spool Files?",
      msg: "This will permanently delete all stored print jobs from the MicroSD storage.",
      confirmText: "Clear All",
      isDanger: true,
      onConfirm: () => {
        fetch('/api/clear_queue', { method: 'POST' }).then(() => {
          loadQueue();
          refresh();
        });
      }
    });
  }

  function refresh() {
    fetch('/api/status')
      .then(r => r.json())
      .then(d => {
        document.getElementById('val-ssid').innerText = d.ssid;
        document.getElementById('val-ip').innerText = d.ip;
        document.getElementById('val-rssi').innerText = d.rssi + ' dBm';
        
        const m = Math.floor(d.uptime / 60);
        const s = d.uptime % 60;
        document.getElementById('val-uptime').innerText = m + 'm ' + s + 's';
        document.getElementById('val-heap').innerText = Math.round(d.freeHeap / 1024) + ' KB';
        document.getElementById('val-jobs').innerText = '#' + d.jobNumber;

        const sdEl = document.getElementById('val-sd-status');
        if (d.sdMounted) {
          sdEl.innerHTML = '<span class="status-pill mounted"><span class="dot-green"></span>Mounted</span>';
          document.getElementById('val-sd-cap').innerText = d.sdTotalMB + ' MB';
        } else {
          sdEl.innerHTML = '<span class="status-pill unmounted"><span class="dot-red"></span>Unmounted</span>';
          document.getElementById('val-sd-cap').innerText = 'No Card';
        }

        const prEl = document.getElementById('val-printer-status');
        if (d.printerConnected) {
          prEl.innerHTML = '<span class="status-pill mounted"><span class="dot-green"></span>Ready</span>';
          document.getElementById('val-printer-model').innerText = d.printerModel;
        } else {
          prEl.innerHTML = '<span class="status-pill unmounted"><span class="dot-red"></span>Disconnected</span>';
          document.getElementById('val-printer-model').innerText = 'None';
        }

        const paperEl = document.getElementById('val-paper-status');
        if (d.paperEmpty) {
          paperEl.innerHTML = '<span class="status-pill unmounted"><span class="dot-red"></span>Out of Paper</span>';
        } else {
          paperEl.innerHTML = '<span class="status-pill mounted"><span class="dot-green"></span>Loaded</span>';
        }

        document.getElementById('picker').value = d.ledColor;
        document.getElementById('color-preview').style.background = d.ledColor;

        if (d.lastCompletedJob > 0 && d.lastCompletedJob !== lastNotifiedJob) {
          if (lastNotifiedJob !== 0) {
            triggerNotification(d.lastCompletedJob, d.totalBytes);
          }
          lastNotifiedJob = d.lastCompletedJob;
        }
      })
      .catch(e => console.log(e));
  }

  function sendColor(hex) {
    document.getElementById('color-preview').style.background = hex;
    fetch('/api/led?hex=' + encodeURIComponent(hex));
  }
  function sendPreset(p) {
    fetch('/api/led?preset=' + encodeURIComponent(p));
  }

  function loadQueue() {
    fetch('/api/queue')
      .then(r => r.json())
      .then(data => {
        const files = data.files || [];
        const scheduled = data.scheduled || [];
        
        document.getElementById('val-queue-count').innerText = files.length + (files.length === 1 ? ' file' : ' files');
        
        const container = document.getElementById('queue-items');
        if (files.length === 0) {
          container.innerHTML = '<div class="row"><span class="row-label" style="color:#8e8e93;">Queue Empty</span><span class="row-value" style="color:#8e8e93;">Idle</span></div>';
        } else {
          let html = '';
          files.forEach(j => {
            const kb = Math.round(j.size / 1024);
            html += '<div class="row">';
            html += '  <div style="display:flex; align-items:center; gap:10px;">';
            html += '    <span class="del-btn" onclick="confirmDeleteJob(\'' + j.name + '\')">&times;</span>';
            html += '    <span class="row-label">' + j.name + '</span>';
            html += '  </div>';
            html += '  <div style="display:flex; align-items:center; gap:8px;">';
            html += '    <span class="row-value" style="color:#8e8e93; font-size:13px;">' + kb + ' KB</span>';
            html += '    <span class="reprint-btn" onclick="promptSchedule(\'' + j.name + '\')" title="Schedule Print">⏰</span>';
            html += '    <span class="reprint-btn" onclick="reprintJob(\'' + j.name + '\')" title="Print Now">↺</span>';
            html += '  </div>';
            html += '</div>';
          });
          container.innerHTML = html;
        }

        const schedContainer = document.getElementById('scheduled-items');
        if (scheduled.length > 0) {
          let sHtml = '<div class="row" style="background:#f9f9fb;"><span class="row-label" style="font-size:13px; font-weight:600; color:#0071e3;">Scheduled Timers</span><span class="row-value" style="font-size:12px; color:#0071e3;">' + scheduled.length + ' active</span></div>';
          scheduled.forEach(s => {
            const m = Math.floor(s.remainingSec / 60);
            const sec = s.remainingSec % 60;
            const timeStr = (m > 0 ? m + 'm ' : '') + sec + 's';
            sHtml += '<div class="row">';
            sHtml += '  <span class="row-label">⏰ ' + s.name + '</span>';
            sHtml += '  <div style="display:flex; align-items:center; gap:10px;">';
            sHtml += '    <span class="row-value" style="color:#ff9500; font-weight:500; font-size:13px;">in ' + timeStr + '</span>';
            sHtml += '    <span class="del-btn" style="background:#8e8e93; font-size:11px;" onclick="cancelSchedule(' + s.id + ')">&times;</span>';
            sHtml += '  </div>';
            sHtml += '</div>';
          });
          schedContainer.innerHTML = sHtml;
          schedContainer.style.display = 'block';
        } else {
          schedContainer.innerHTML = '';
          schedContainer.style.display = 'none';
        }
      })
      .catch(e => console.log(e));
  }

  setInterval(refresh, 1500);
  setInterval(loadQueue, 2000);
  refresh();
  loadQueue();
</script>

</body>
</html>
)rawliteral";
    webServer.send(200, "text/html", html);
}

// -------------------------------------------------------------
// Spooling: WiFi TCP -> SD Card (or Direct Stream)
// -------------------------------------------------------------
String spoolJobToSD(WiFiClient &c) {
    jobNumber++;
    currentStatusStr = "Receiving Job #" + String(jobNumber);
    String path = "/spool/job_" + String(jobNumber) + ".raw";

    if (sdMounted && !SD.exists("/spool")) {
        SD.mkdir("/spool");
    }

    File f;
    if (sdMounted) {
        f = SD.open(path, FILE_WRITE);
    }

    uint8_t buffer[1024];
    uint32_t totalBytes = 0;
    unsigned long lastDataTime = millis();

    while (c.connected() || c.available()) {
        int avail = c.available();
        if (avail > 0) {
            int toRead = min(avail, (int)sizeof(buffer));
            int bytesRead = c.read(buffer, toRead);
            if (bytesRead > 0) {
                if (f) {
                    f.write(buffer, bytesRead);
                } else if (printerConnected) {
                    printerDev.write(buffer, bytesRead);
                }
                totalBytes += bytesRead;
                lastDataTime = millis();
            }
        } else {
            if (millis() - lastDataTime > 2500) {
                break;
            }
            delay(5);
        }
        yield();
    }

    bool savedToFile = false;
    if (f) {
        f.flush();
        f.close();
        savedToFile = true;
    }

    totalBytesReceived += totalBytes;
    if (!savedToFile) {
        lastCompletedJob = jobNumber;
    }
    return savedToFile ? path : "";
}

void printFromSD(String path) {
    if (path.length() == 0) return;
    currentStatusStr = "Streaming to USB...";
    Serial.printf("[Print] Streaming %s to printer...\n", path.c_str());

    if (!sdMounted) return;
    File f = SD.open(path, FILE_READ);
    if (!f) return;

    uint8_t buffer[512];
    size_t totalStreamed = 0;
    while (f.available()) {
        int bytesRead = f.read(buffer, sizeof(buffer));
        if (bytesRead > 0) {
            if (printerConnected) {
                printerDev.write(buffer, bytesRead);
            }
            totalStreamed += bytesRead;
        }
        yield();
    }
    f.close();

    Serial.printf("[Print] Streamed %u bytes to printer.\n", totalStreamed);
    lastCompletedJob = jobNumber;
    currentStatusStr = "Ready";
}

// Static File Upload Tracking
static File currentUploadFile;
static String currentUploadPath = "";
static String currentUploadSafeName = "";

// -------------------------------------------------------------
// Main Setup
// -------------------------------------------------------------
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n========================================");
    Serial.println("       PrintBridge Starting Up...       ");
    Serial.println("========================================");

    // WS2812B RGB Setup
    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(45);
    setStatusColor(CRGB::Blue);

    // MicroSD Init
    initSDCard();

    // Connect WiFi
    Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
#if USE_STATIC_IP
    IPAddress staticIP(STATIC_IP);
    IPAddress staticGateway(STATIC_GATEWAY);
    IPAddress staticSubnet(STATIC_SUBNET);
    IPAddress staticDNS(STATIC_DNS);
    WiFi.config(staticIP, staticGateway, staticSubnet, staticDNS);
#endif
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 25) {
        delay(400);
        setStatusColor((attempts % 2 == 0) ? CRGB::Yellow : CRGB::Black);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WiFi] Connected! IP Address: ");
        Serial.println(WiFi.localIP());

        if (MDNS.begin("printbridge")) {
            MDNS.addService("http", "tcp", 9101);
            MDNS.addService("pdl-datastream", "tcp", 9100);
            Serial.println("[MDNS] Active at http://printbridge.local:9101");
        }

        printServer.begin();
        Serial.println("[PrintServer] Listening on Port 9100");

        webServer.on("/", handleRoot);
        webServer.on("/api/status", handleApiStatus);
        webServer.on("/api/queue", handleApiQueue);
        webServer.on("/api/clear_queue", handleApiClearQueue);
        webServer.on("/api/delete_job", handleApiDeleteJob);
        webServer.on("/api/reprint", handleApiReprint);
        webServer.on("/api/schedule", handleApiSchedule);
        webServer.on("/api/cancel_schedule", handleApiCancelSchedule);
        webServer.on("/api/copy", handleApiCopy);
        webServer.on("/api/led", handleApiLED);

        // Direct File Upload to Print (Immediate or Scheduled)
        webServer.on("/api/upload_print", HTTP_POST, []() {
            uint32_t delaySec = 0;
            if (webServer.hasArg("delay_sec")) {
                delaySec = webServer.arg("delay_sec").toInt();
            }
            if (currentUploadSafeName.length() > 0) {
                if (delaySec > 0) {
                    uint32_t nowSec = millis() / 1000;
                    for (int i = 0; i < MAX_SCHEDULED; i++) {
                        if (!scheduledJobs[i].active) {
                            scheduledJobs[i].filename = currentUploadSafeName;
                            scheduledJobs[i].executeAtSeconds = nowSec + delaySec;
                            scheduledJobs[i].active = true;
                            Serial.printf("[Schedule] Uploaded job %s scheduled in %lu seconds\n", currentUploadSafeName.c_str(), (unsigned long)delaySec);
                            break;
                        }
                    }
                } else {
                    if (sdMounted && SD.exists(currentUploadPath)) {
                        printFromSD(currentUploadPath);
                    }
                }
            }
            webServer.sendHeader("Access-Control-Allow-Origin", "*");
            webServer.send(200, "application/json", "{\"uploaded\":true}");
        }, []() {
            HTTPUpload& upload = webServer.upload();
            if (upload.status == UPLOAD_FILE_START) {
                jobNumber++;
                String cleanName = upload.filename;
                cleanName.replace("/", "_");
                cleanName.replace("\\", "_");
                cleanName.replace(" ", "_");
                if (cleanName.length() == 0) cleanName = "document.raw";
                currentUploadSafeName = "job_" + String(jobNumber) + "_" + cleanName;
                currentUploadPath = "/spool/" + currentUploadSafeName;
                Serial.printf("[Upload] Receiving: %s -> %s\n", upload.filename.c_str(), currentUploadPath.c_str());
                if (sdMounted) {
                    if (!SD.exists("/spool")) SD.mkdir("/spool");
                    currentUploadFile = SD.open(currentUploadPath, FILE_WRITE);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (currentUploadFile) {
                    currentUploadFile.write(upload.buf, upload.currentSize);
                } else if (printerConnected) {
                    printerDev.write(upload.buf, upload.currentSize);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (currentUploadFile) {
                    currentUploadFile.flush();
                    currentUploadFile.close();
                    Serial.printf("[Upload] Saved %u bytes to %s\n", upload.totalSize, currentUploadPath.c_str());
                }
                totalBytesReceived += upload.totalSize;
            }
        });

        // 1-Click Windows Setup Script Download (Dynamic Host Detection)
        webServer.on("/api/install.cmd", HTTP_GET, []() {
            String host = "192.168.18.52";
            String hHeader = webServer.hostHeader();
            if (hHeader.length() > 0) {
                int colon = hHeader.indexOf(':');
                if (colon != -1) hHeader = hHeader.substring(0, colon);
                if (hHeader.length() > 0 && hHeader != "printbridge.local") {
                    host = hHeader;
                }
            }

            String portName = (host == "192.168.18.52") ? "PrintBridge" : ("PrintBridge_" + host);

            String script = "@echo off\r\n";
            script += "title PrintBridge Installer\r\n";
            script += "echo ========================================================\r\n";
            script += "echo       PrintBridge HP LaserJet 1-Click Installer         \r\n";
            script += "echo ========================================================\r\n";
            script += "echo Target Host: " + host + " (Port 9100)\r\n";
            script += "echo Adding network printer port (" + portName + ")...\r\n";
            script += "powershell -NoProfile -Command \"$p = Get-PrinterPort -Name '" + portName + "' -ErrorAction SilentlyContinue; if (!$p) { Add-PrinterPort -Name '" + portName + "' -PrinterHostAddress '" + host + "' }\"\r\n";
            script += "echo Setting up HP LaserJet driver queue...\r\n";
            script += "powershell -NoProfile -Command \"$pr = Get-Printer -Name 'PrintBridge HP LaserJet' -ErrorAction SilentlyContinue; if (!$pr) { Add-Printer -Name 'PrintBridge HP LaserJet' -DriverName 'HP LaserJet Pro MFP M127-M128 PCLmS' -PortName '" + portName + "' } else { Set-Printer -Name 'PrintBridge HP LaserJet' -PortName '" + portName + "' }\"\r\n";
            script += "echo.\r\n";
            script += "echo [SUCCESS] PrintBridge HP LaserJet is connected via " + host + "!\r\n";
            script += "echo You can now select it in Word, Chrome, Adobe, or anywhere.\r\n";
            script += "echo ========================================================\r\n";
            script += "pause\r\n";
            webServer.sendHeader("Content-Disposition", "attachment; filename=\"Install_PrintBridge_Printer.cmd\"");
            webServer.send(200, "application/octet-stream", script);
        });

        // Wireless OTA Firmware Update over WiFi
        webServer.on("/update", HTTP_POST, []() {
            webServer.sendHeader("Connection", "close");
            webServer.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
            ESP.restart();
        }, []() {
            HTTPUpload& upload = webServer.upload();
            if (upload.status == UPLOAD_FILE_START) {
                Serial.printf("[OTA] Update: %s\n", upload.filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] Success: %u bytes\n", upload.totalSize);
                } else {
                    Update.printError(Serial);
                }
            }
        });

        webServer.begin();
        Serial.println("[WebServer] Listening on Port 9101");

        // USB Host Controller Init
        usbHost.begin();
        Serial.println("[USB] USB Host Controller Initialized.");

        currentStatusStr = "Ready";
        setStatusColor(CRGB::Green);
    } else {
        Serial.println("[WiFi] Connection failed! Check credentials.");
        currentStatusStr = "WiFi Error";
        setStatusColor(CRGB::Red);
    }
}

// -------------------------------------------------------------
// Main Loop
// -------------------------------------------------------------
void loop() {
    // Check USB printer enumeration & status (every 1s)
    static unsigned long lastUSBCheck = 0;
    if (millis() - lastUSBCheck > 1000) {
        lastUSBCheck = millis();
        if (!printerConnected) {
            if (printerDev.begin()) {
                printerConnected = true;
                char mdl[64] = "USB Printer";
                if (printerDev.deviceIdField("MDL", mdl, sizeof(mdl)) > 0) {
                    printerModelStr = String(mdl);
                } else {
                    printerModelStr = "Ready";
                }
                Serial.printf("[USB] Printer connected: %s\n", printerModelStr.c_str());
            }
        } else {
            if (!printerDev.ready()) {
                printerConnected = false;
                printerModelStr = "Disconnected";
                paperEmpty = false;
                printerDev.end();
                Serial.println("[USB] Printer disconnected.");
            } else {
                // Read live paper status from USB device
                printer::PortStatus ps;
                if (printerDev.readPortStatus(ps) && !ps.unknown) {
                    paperEmpty = ps.paperEmpty;
                    printerSelected = ps.selected;
                    printerError = ps.error;
                }
            }
        }
    }

    // Check Scheduled Prints (every 1s)
    static unsigned long lastSchedCheck = 0;
    if (millis() - lastSchedCheck > 1000) {
        lastSchedCheck = millis();
        uint32_t nowSec = millis() / 1000;
        for (int i = 0; i < MAX_SCHEDULED; i++) {
            if (scheduledJobs[i].active && nowSec >= scheduledJobs[i].executeAtSeconds) {
                scheduledJobs[i].active = false;
                String path = "/spool/" + scheduledJobs[i].filename;
                Serial.printf("[Schedule] Triggering scheduled print for: %s\n", path.c_str());
                if (sdMounted && SD.exists(path)) {
                    printFromSD(path);
                }
            }
        }
    }

    // Handle Port 9101 Web Requests
    webServer.handleClient();

    // Periodic SD Card Hot-Plug Check (every 2.5s)
    static unsigned long lastSDCheck = 0;
    if (millis() - lastSDCheck > 2500) {
        lastSDCheck = millis();
        if (sdMounted) {
            File root = SD.open("/");
            if (!root) {
                sdMounted = false;
                sdTotalMB = 0;
                sdUsedMB = 0;
                SD.end();
                Serial.println("[SD] Card removed.");
            } else {
                root.close();
            }
        } else {
            if (SD.begin(SD_CS, sdSPI, 4000000)) {
                sdMounted = true;
                sdTotalMB = SD.totalBytes() / (1024 * 1024);
                sdUsedMB = SD.usedBytes() / (1024 * 1024);
                if (!SD.exists("/spool")) SD.mkdir("/spool");
                Serial.printf("[SD] Card detected! Total: %llu MB\n", sdTotalMB);
            }
        }
    }

    // Periodic Serial Heartbeat (every 5s)
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 5000) {
        lastHeartbeat = millis();
        Serial.printf("[PrintBridge] IP: %s | WiFi RSSI: %d dBm | SD: %s | Status: %s\n",
            WiFi.localIP().toString().c_str(),
            WiFi.RSSI(),
            sdMounted ? "Mounted" : "Unmounted",
            currentStatusStr.c_str());
    }

    // Handle Port 9100 Print Server
    client = printServer.available();
    if (client) {
        setStatusColor(CRGB::Cyan);

        String jobFile = spoolJobToSD(client);
        client.stop();

        if (jobFile.length() > 0) {
            setStatusColor(CRGB::Purple);
            printFromSD(jobFile);
        }

        currentStatusStr = "Ready";
        setStatusColor(CRGB::Green);
    }

    delay(2);
}
