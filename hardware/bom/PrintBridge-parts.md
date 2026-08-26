# PrintBridge — parts list (all real, verified in KiCad)

Open `hardware/PrintBridge.kicad_pro` in KiCad 9 — two project libraries load automatically:
`PrintBridge` (official Espressif) and `PrintBridge_JLC` (LCSC/JLCPCB-matched). All entries
below were checked to parse correctly and carry a real 3D model (verified programmatically,
same file format KiCad itself reads).

## Sourced parts (symbol + footprint + real 3D)
| Ref | Part | Symbol | LCSC | Notes |
|---|---|---|---|---|
| U1 | ESP32-S3-WROOM-1 | PrintBridge:ESP32-S3-WROOM-1 | (order via distributor) | Official Espressif STEP model, ~19.6x26.6mm |
| J1 | USB-A host receptacle | PrintBridge_JLC:USB-A-F-90 | C168708 | THT, easy hand-solder. To printer. |
| J2 | USB-C power input | PrintBridge_JLC:TYPE-C-31-M-12 | C165948 | 5V in only, no data lines used |
| U2 | AMS1117-3.3 | PrintBridge_JLC:AMS1117-3.3 | C6186 | 5V -> 3.3V |
| SW1, SW2 | Tactile switch | PrintBridge_JLC:TS-1187A-B-A-B | C318884 | BOOT + RESET(EN) |

## Built-in KiCad parts (ship with KiCad 9, real bundled 3D, no sourcing needed)
| Ref | Part | Symbol | Footprint | Value/notes |
|---|---|---|---|---|
| R1 | Pull-up | Device:R | R_0603_1608Metric | 10k, EN to 3V3 |
| R2 | Pull-up | Device:R | R_0603_1608Metric | 10k, GPIO0/BOOT to 3V3 |
| R3 | CC1 pull-down | Device:R | R_0603_1608Metric | 5.1k (USB-C power negotiation) |
| R4 | CC2 pull-down | Device:R | R_0603_1608Metric | 5.1k |
| R5 | LED series | Device:R | R_0603_1608Metric | ~330R |
| C1 | Decoupling | Device:C | C_0603_1608Metric | 100nF, at 3V3 pin |
| C2 | Bulk | Device:C | C_0805_2012Metric | 10uF, at 3V3 pin |
| D1 | Status LED | Device:LED | LED_0603_1608Metric | any color |
| F1 | Polyfuse | Device:Polyfuse | Fuse_1206_3216Metric | 500mA-1A, USB-C 5V in |
| J3 | UART header | Connector_Generic:Conn_01x04 | PinHeader_2.54mm_1x04 | 3V3/GND/TX/RX, for external USB-serial flasher |

## Wiring notes (from real pin data on the official symbol)
- Pin 3 = EN (reset): 10k pull-up to 3V3 + SW2 to GND.
- Pin 27 = GPIO0/BOOT: 10k pull-up to 3V3 + SW1 to GND.
- Pins 13/14 = GPIO19/20 = USB_D-/D+ (the module's *only* USB pin pair) -> wired to J1 (USB-A host, printer). Not shared with any other USB port.
- Pins 36/37 = U0RXD/U0TXD -> J3 UART header, for flashing via external USB-serial adapter.
- J1 VBUS -> tied to raw 5V rail (before the regulator), through F1. Host mode must supply 5V to the printer.
- J2 5V -> F1 -> 5V rail -> U2 (AMS1117) -> 3V3 rail -> U1, R1, R2, C1/C2.
- J2 CC1/CC2 -> R3/R4 (5.1k to GND) so any standard USB-C charger recognizes it as a sink.
- Pins 1, 40, 41 = GND.

## Not included (by design, keeps cost down)
- No onboard USB-serial chip - use an external $3 USB-to-UART dongle on J3 for flashing.
- No USB mux IC - single USB port is permanently Host mode.

Board size estimate: ~45 x 28-30mm.
