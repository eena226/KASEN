# demo/ : the test and demo build (3 x HX711 + button simulation)

This folder will hold the demo firmware: `main.c`, `hx711_sim.c`, `CMakeLists.txt`, `prj.conf`, `app.overlay`. For now it holds the wiring plan.

The demo runs the same `sensing.c`, `logic*.c`, `comms*.c` and `ui.c` as the station. Only the ADC backend is different:

| | Station build | Demo build |
|---|---|---|
| Bins 1 to 3 | ADS1261 + INA333 | one HX711 each, load cell wired straight to it |
| Bins 4 and 5 | ADS1261 + INA333 | simulated load cells driven by buttons |
| Display, Wi-Fi, MQTT, buttons | FBD pins | same FBD pins |

A simulated bin fakes the raw ADC reading, not the count. It still has to be tared, calibrated and given a sample quantity from the hub, so the demo exercises every routine on the process maps. Which bins are real and which are simulated is one line in `app.overlay`.

## Pin-to-pin

### Three HX711 boards (bins 1, 2, 3)

The three boards share one clock wire and have one data wire each. The HX711 pins are not on the FBD (the final board has no HX711), so I chose pins the FBD leaves free and that are plain GPIOs on the DK header.

| Signal | nRF54LM20 pin | Connects to |
|---|---|---|
| HX_SCK (shared) | P1.10 | SCK (PD_SCK) of all three HX711 boards |
| HX_DOUT1 | P1.11 | DT (DOUT) of the bin 1 board |
| HX_DOUT2 | P1.12 | DT (DOUT) of the bin 2 board |
| HX_DOUT3 | P1.14 | DT (DOUT) of the bin 3 board |

Each HX711 board:

| HX711 board pin | Connects to | Note |
|---|---|---|
| VCC | +3.3 V (the same rail as the MCU's I/O) | Not 5 V. DT swings to the board's supply and the nRF pins are not 5 V tolerant |
| GND | GND | |
| E+ | Load cell E+ (often red) | Wire colours vary; check your cell |
| E− | Load cell E− (often black) | |
| A+ | Load cell SIG+ (often green) | |
| A− | Load cell SIG− (often white) | |

If a board has separate VDD and VCC pins (SparkFun style), VDD goes to 3.3 V and VCC may go to 5 V.

Leave each board's RATE setting at 10 samples per second (the usual default). That matches `KASEN_SENSE_RATE_HZ`.

How the read works, briefly: each chip pulls its data line low when a reading is ready and holds it. When all three are low, the firmware sends 25 clock pulses and reads the three data lines after each one. The steps are in the `sensing_backend_demo` comment in `sensing.h`.

### Display, Wi-Fi, buttons

Identical to `station/README_Station_Wiring.md`: display on P2.01 (SCK), P2.02 (MOSI), P0.05 (CS), P0.02 (DC), P0.06 (RST), P0.07 (BL); nRF7002 EB II on its shield pins; six buttons on P0.08, P0.09, P1.29, P1.30, P1.31, P1.00.

The ADS1261 pins (P2.04, P2.05, P1.08, P1.09) are unused in the demo.

The three DK notes at the end of `station/README_Station_Wiring.md` apply here too. In the demo the display still needs P2.01 and P2.02, which the DK does not bring to its header by default.

## Buttons

| Button | Pin | Demo meaning | Event in `ui.h` |
|---|---|---|---|
| BTN1 | P0.08 | Start Assembly | `UI_BTN_START_ASSEMBLY` |
| BTN2 | P0.09 | Assembly Complete | `UI_BTN_ASSEMBLY_COMPLETE` |
| BTN3 | P1.29 | Select the other simulated bin (4 ↔ 5, highlighted on the LCD) | `UI_BTN_SIM_NEXT_BIN` |
| BTN4 | P1.30 | Take one part out of the selected bin; repeats while held | `UI_BTN_SIM_PART_TAKE` |
| BTN5 | P1.31 | Put one part into the selected bin; repeats while held | `UI_BTN_SIM_PART_ADD` |
| BTN6 | P1.00 | Place or remove the 100 g reference weight on the selected bin | `UI_BTN_SIM_REF_WEIGHT` |

BTN1 and BTN2 mean the same as in the station build. BTN6 is how a simulated bin is calibrated: send calibrate with 100 g from the hub, wait for the LCD prompt, press BTN6, and press it again after "Done".

## A demo run

1. From the hub, for a real bin (say bin 1): assign a part, tare with the bin empty, calibrate with a real known weight, put in a counted handful of parts and send the sample quantity.
2. From the hub, for a simulated bin (say bin 4): assign a part, tare, calibrate with 100 g using BTN6, press BTN5 ten times and send sample quantity 10.
3. From the hub: load a recipe, for example 2 parts from bin 1 and 1 part from bin 4.
4. Press BTN1 (Start). Take two real parts from bin 1 and press BTN4 once. Press BTN2 (Complete). The LCD shows PASS and the hub gets a pass result.
5. Repeat but take three parts from bin 1. The LCD flags bin 1 and the hub gets a mismatch alarm.
6. Keep removing parts until a count reaches its low level. The low-stock alarm appears.

## Building (planned; not possible until the source files exist)

```
west build -b nrf54lm20dk/nrf54lm20a/cpuapp demo -- -Ddemo_SHIELD=nrf7002eb2
```

Same caveats as the station build.
