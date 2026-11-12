# station/ : the actual build (ADS1261 + INA333)

This folder will hold the real station firmware: `main.c`, `ads1261.c`, `CMakeLists.txt`, `prj.conf`, `app.overlay`. For now it holds the wiring plan.

There are two pin maps. **Map A** is your FBD, for your own PCB. **Map B** is what I propose for prototyping on the nRF54LM20 DK, because several Map A pins are already used by the DK itself. The C code is identical for both; only `app.overlay` differs.

## Analog chain (same in Map A and Map B)

One chain per bin, five in total. Transcribed from the FBD.

| From | To | Note |
|---|---|---|
| Load cell E+ | +5 V | Excitation |
| Load cell E− | GND | |
| Load cell SIG− | INA333 pin 2 (VIN−) | |
| Load cell SIG+ | INA333 pin 3 (VIN+) | |
| INA333 pin 1 (RG) ↔ pin 8 (RG) | 402 Ω resistor R1 | Gain ≈ 250 |
| INA333 pin 7 (V+) | +5 V | |
| INA333 pin 4 (V−) | GND | |
| INA333 pin 5 (REF) | GND | See the top README, finding 9 |
| INA333 pin 6 (VOUT) | Anti-alias filter IN | RC, cutoff still to be designed |
| Anti-alias filter OUT | ADS1261 AINx | Table below |

| Bin (LCD / MQTT) | Bin index in C | ADS1261 input | ADS1261 pin |
|---|---|---|---|
| 1 | 0 | AIN0 | 32 |
| 2 | 1 | AIN1 | 31 |
| 3 | 2 | AIN2 | 30 |
| 4 | 3 | AIN3 | 29 |
| 5 | 4 | AIN4 | 28 |

| ADS1261 pin | Connects to |
|---|---|
| 1 AINCOM | GND (the negative input for every bin) |
| 4 AVDD | +5 V (also the ADC reference) |
| 5 AVSS | GND |
| 17 DVDD | +3.3 V |
| 16 DGND | GND |
| 15 BYPASS | Capacitor to DGND |
| 2 CAPP ↔ 3 CAPN | Capacitor between them |

Firmware settings that follow from this wiring: single-ended reads (AINx against AINCOM), reference = AVDD, ADC gain 1, internal clock, SPI mode 1.

## Map A: your PCB (the FBD as drawn)

| Signal | nRF54LM20A pin (ball) | Other end |
|---|---|---|
| SPI_SCK | P2.01 (C10) | ADS1261 pin 11 SCLK; display CK |
| SPI_MOSI | P2.02 (D10) | ADS1261 pin 12 DIN; display SI |
| SPI_MISO | P2.04 (F10) | ADS1261 pin 14 DOUT/DRDY |
| ADC_CS | P2.05 (G9) | ADS1261 pin 10 CS |
| ADC_DRDY | P1.08 (F8) | ADS1261 pin 13 DRDY |
| ADC_RESET | P1.09 (E8) | ADS1261 pin 8 RESET |
| TFT_CS | P0.05 (H3) | Display TC |
| TFT_DC | P0.02 (J3) | Display DC |
| TFT_RST | P0.06 (H2) | Display RT |
| TFT_BL | P0.07 (G3) | Display BL |
| BTN1 to BTN6 | P0.08, P0.09, P1.29, P1.30, P1.31, P1.00 | Push button to GND, pull-up to +3.3 V |
| WIFI SCK / MOSI / MISO / CS | P3.03 / P3.00 / P3.01 / P3.02 | nRF7002 CLK / D0 / D1 / CS |
| WIFI IRQ | P1.05 | nRF7002 IRQ |
| WIFI BUCKEN | P1.04 | nRF7002 BUCKEN |
| WIFI IOVDDEN | P1.13 | nRF7002 IOVDDEN |
| WIFI GRANT / REQ / STATUS | P1.07 / P0.04 / P0.03 | nRF7002 coexistence pins |

Display (Adafruit 5394, ST7789, 320x170): VIN to +3.3 V, GND to GND, plus the four TFT signals and the two SPI signals above. The display's MISO is not connected.

Power: 5 V in → TLV733P → 3.3 V. The 5 V rail feeds the load cells, INA333s and ADS1261 AVDD. The 3.3 V rail feeds ADS1261 DVDD, the MCU and the display.

One thing to confirm for the PCB: ADC_DRDY needs a pin that can raise an interrupt. P1.08 is on port 1, which can; port 2 pins cannot on this chip family, so do not move DRDY to port 2.

## Map B: nRF54LM20 DK prototype (proposed)

Why Map A cannot be used on the DK unchanged:

| Map A pin | On the DK it is already |
|---|---|
| P1.09, P1.08, P0.05 | Button 1, Button 2, Button 3 |
| P2.01, P2.02, P2.04, P2.05 | The onboard flash chip's clock, data and chip-select; not connected to the pin header by default |
| P0.06 to P0.09 | Debugger serial port 0; the debugger drives P0.07 and P0.09 |

Proposed wiring. Every pin below is listed in the DK user guide as a plain GPIO on the pin headers with no other function. I have not had a DK in hand, so check each against the silkscreen before wiring.

| Signal | DK pin | Other end | Same as Map A? |
|---|---|---|---|
| SPI_SCK | P1.10 | ADS1261 SCLK; display CK | moved |
| SPI_MOSI | P1.11 | ADS1261 DIN; display SI | moved |
| SPI_MISO | P1.12 | ADS1261 DOUT/DRDY | moved |
| ADC_CS | P1.14 | ADS1261 CS | moved |
| ADC_DRDY | P1.15 | ADS1261 DRDY | moved |
| ADC_RESET | P1.23 | ADS1261 RESET | moved |
| TFT_CS | P1.24 | Display TC | moved |
| TFT_DC | P0.02 | Display DC | same |
| TFT_RST | P1.29 | Display RT | moved |
| TFT_BL | P1.30 | Display BL | moved |
| Wi-Fi (all) | Expansion header | nRF7002 EB II plugged in | same |

In the overlay this bus is SPI instance `spi21`. `spi22` is taken by the Wi-Fi shield and `spi20` shares hardware with the console UART.

Spare free pins if needed: P1.00, P0.00, P0.01.

### Buttons and LEDs (DK built-in)

| DK part | Pin | Station build meaning |
|---|---|---|
| Button 0 | P1.26 | Start Assembly |
| Button 1 | P1.09 | Assembly Complete |
| Button 2 | P1.08 | not used |
| Button 3 | P0.05 | not used |
| LED 0 | P1.22 | Link: blinking = connecting, on = connected to broker |
| LED 1 | P1.25 | Assembly cycle active |
| LED 2 | P1.27 | An alarm is active |
| LED 3 | P1.28 | Alive (toggles) |

### Power and voltage on the DK

- Take +5 V for the load cells, INA333s and ADS1261 AVDD from a DK 5 V header pin.
- Power ADS1261 DVDD and the display VIN from the DK's **VDD:IO** pin, not from a separate 3.3 V regulator. The nRF pins are not 5 V tolerant and should see the same logic level they are powered at.
- Measure VDD:IO first. It is adjustable from 1.8 V to 3.3 V in the Board Configurator app; the ADS1261 needs at least 2.7 V on DVDD. Set it to 3.3 V if it is lower.
- Join the grounds of the DK and the analog board at one point near the ADC.

## Wi-Fi

The nRF7002 EB II plugs into the DK's expansion header; there is nothing to wire. Its pins are defined by the shield and match the FBD.

Before the first build, fetch the Wi-Fi firmware blobs once: `west blobs fetch nrf_wifi`.

## Building (planned; not possible until the source files exist)

```
west build -b nrf54lm20dk/nrf54lm20a/cpuapp station -- -Dstation_SHIELD=nrf7002eb2
```

Use `nrf7002eb2_nrf7001` instead if the board carries an nRF7001 (2.4 GHz only). The argument form follows Nordic's MQTT sample for this board pair; I have not run it.
