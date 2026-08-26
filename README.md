# PrintBridge

Im so done with needing to unplug all my cords and plug it all in ontip of a file of boxes to print one god damn thing so i made my wired printer wireless

## what is this thing

A small, cheap module that turns a wired USB printer into a WiFi network printer. It plugs into the printer's USB port as a USB **Host**, and exposes it on the network via raw JetDirect (port 9100) printing — works natively with macOS/Windows/Linux print dialogs, no drivers needed on the printer side.

![front render](images/render_front.png)

![back render](images/render_back.png)

## pcb layout

![pcb front](images/pcb_front.png)

![pcb back](images/pcb_back.png)

## schematic

![schematic](images/schematic.png)

## specs

- ESP32-S3-WROOM-1 module for WiFi and native USB Host
- USB-A host receptacle to connect to printer
- USB-C for power input (5V only)
- Status LED
- BOOT and EN (RESET) tactile switches
- 1x4 UART header for initial firmware flashing
- Modular certification (FCC/CE from Espressif module)
- Cheap to build (~$3.50 in components)

## wiring

**USB-C power in:** provides power to the board and supplies 5V directly to the printer (Host mode must supply power). CC lines have 5.1k pull-downs to negotiate power from smart chargers. 

**USB-A host:** the D+ and D- lines are permanently wired to the ESP32-S3's single USB port (GPIO19/GPIO20).

**Flashing:** there's no onboard USB-serial chip or USB mux to keep costs down. You'll need to use an external USB-to-UART dongle connected to the 4-pin UART header (3V3/GND/TX/RX) to flash firmware.

## BOM

| Ref | Qty | Value | Package | Description | Unit Price | Subtotal | Link |
|-----|-----|-------|---------|-------------|------------|----------|------|
| U1 | 1 | ESP32-S3-WROOM-1 | WROOM | ESP32-S3 WiFi/BLE Module | ~$3.00 | ~$3.00 | |
| J1 | 1 | USB-A host receptacle | Through-hole | USB-A female 90-degree (C168708) | ~$0.15 | ~$0.15 | [LCSC](https://www.lcsc.com/search?q=C168708) |
| J2 | 1 | USB-C power input | SMD | Type-C 12-pin (C165948) | ~$0.10 | ~$0.10 | [LCSC](https://www.lcsc.com/search?q=C165948) |
| U2 | 1 | AMS1117-3.3 | SOT-223 | 3.3V Linear Regulator (C6186) | ~$0.05 | ~$0.05 | [LCSC](https://www.lcsc.com/search?q=C6186) |
| SW1, SW2 | 2 | Tactile switch | SMD | TS-1187A-B-A-B (C318884) | ~$0.02 | ~$0.04 | [LCSC](https://www.lcsc.com/search?q=C318884) |
| R1, R2 | 2 | 10k | 0603 | SMD resistor | ~$0.01 | ~$0.02 | |
| R3, R4 | 2 | 5.1k | 0603 | SMD resistor | ~$0.01 | ~$0.02 | |
| R5 | 1 | 330R | 0603 | SMD resistor | ~$0.01 | ~$0.01 | |
| C1 | 1 | 100nF | 0603 | Ceramic decoupling capacitor | ~$0.01 | ~$0.01 | |
| C2 | 1 | 10uF | 0805 | Ceramic bulk capacitor | ~$0.02 | ~$0.02 | |
| D1 | 1 | LED | 0603 | Status LED | ~$0.02 | ~$0.02 | |
| F1 | 1 | Polyfuse | 1206 | 500mA-1A PTC fuse | ~$0.10 | ~$0.10 | |
| J3 | 1 | Conn_01x04 | 2.54mm | UART header | ~$0.05 | ~$0.05 | |

**total: 15 parts — ~$3.59**

## license

do whatever you want with it honestly

---

made by Jonah Novoseller (Kingdragoncat)
