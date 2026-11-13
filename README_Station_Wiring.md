# station/ : the actual build (ADS1261 + INA333)

This folder will hold the real station firmware: `main.c`, `ads1261.c`, `CMakeLists.txt`, `prj.conf`, `app.overlay`. For now it holds the wiring plan.

**Decided:** the pin map is the FBD, for your own PCB. Nothing is moved to suit the DK. Pins appear in one place only, `app.overlay`; no C file contains a pin number.

## Analog chain

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
| INA333 pin 5 (REF) | GND | See README_Overview.md, finding 8 |
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

## Digital pin-to-pin

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
| WIFI SCK / MOSI / MISO / CS | P3.03 / P3.00 / P3.01 / P3.02 | nRF7002 CLK / D0 / D1 / CS |
| WIFI IRQ | P1.05 (H8) | nRF7002 IRQ |
| WIFI BUCKEN | P1.04 (J9) | nRF7002 BUCKEN |
| WIFI IOVDDEN | P1.13 (C3) | nRF7002 IOVDDEN |
| WIFI GRANT / REQ / STATUS | P1.07 / P0.04 / P0.03 | nRF7002 coexistence pins |

In the overlay the sensor and display bus is SPI instance `spi00` (the port 2 pins) and Wi-Fi is `spi22`. The Wi-Fi pins match Nordic's `nrf7002eb2` shield definition exactly, so the shield file can be used unchanged.

Display (Adafruit 5394, ST7789, 320x170): VIN to +3.3 V, GND to GND, plus the four TFT signals and SCK and MOSI above. The display's MISO is not connected.

ADC_DRDY must be on a pin that can raise an interrupt. P1.08 can. Port 2 pins cannot on this chip family, so if the layout changes, do not move DRDY to port 2.

## Buttons

Six push buttons, each from the pin to GND with a pull-up to +3.3 V (pressed = low).

| Button | Pin (ball) | Station build meaning |
|---|---|---|
| BTN1 | P0.08 (F3) | Start Assembly |
| BTN2 | P0.09 (E3) | Assembly Complete |
| BTN3 | P1.29 (H10) | spare |
| BTN4 | P1.30 (J10) | spare |
| BTN5 | P1.31 (K9) | spare |
| BTN6 | P1.00 (K8) | spare |

The scope defines only Start and Complete. BTN3 to BTN6 are unassigned in the station build until you say what they are for; the demo build uses them for the simulator.

## Power

5 V in → TLV733P → 3.3 V. The 5 V rail feeds the load cells, INA333s and ADS1261 AVDD. The 3.3 V rail feeds ADS1261 DVDD, the MCU and the display.

## If this build is run on the DK before the PCB exists

You said to ignore the DK's buttons and flash, so nothing above changes. These are the three things that will bite on a DK with the FBD pins, so nobody loses a day to them:

1. **P2.01, P2.02, P2.04, P2.05 go to the DK's onboard flash chip and are not connected to the pin header by default.** P2.05 is that chip's select line, so with the flash still attached, every ADC read would select the flash too. The DK user guide's solder-bridge and external-memory sections say how to route these pins to the header.
2. **P0.06 to P0.09 are the debugger's serial port 0.** The debugger drives P0.07 (TFT_BL) and P0.09 (BTN2). Turn that port off in the Board Configurator app.
3. **P1.09, P1.08 and P0.05 have DK Buttons 1, 2 and 3 on them.** They do no harm unless pressed: Button 1 would reset the ADC, Button 2 would fake a data-ready, Button 3 would select the display.

Also set the DK's VDD:IO to 3.3 V in the Board Configurator and power ADS1261 DVDD and the display from it, since the nRF pins are not 5 V tolerant.

## Wi-Fi

nRF7002 EB II. On the DK it plugs into the expansion header with nothing to wire. Fetch the Wi-Fi firmware blobs once before the first build: `west blobs fetch nrf_wifi`.

## Building (planned; not possible until the source files exist)

```
west build -b nrf54lm20dk/nrf54lm20a/cpuapp station -- -Dstation_SHIELD=nrf7002eb2
```

The argument form follows Nordic's MQTT sample for this board pair; I have not run it. Your PCB will later need its own board definition in place of `nrf54lm20dk`.
