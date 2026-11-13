# KASEN station firmware: headers and plan

**Status:** headers only. The six header files compile on their own as C11 and C++ with all warnings on. No function has a body yet, and nothing here has run on hardware.

Read this file first, then `station/README_Station_Wiring.md` and `demo/README_Demo_Wiring.md` for the pin-to-pin wiring.

## What is in this folder

```
kasen-firmware/
├── README_Overview.md         this file
├── include/kasen/             shared by both builds
│   ├── kasen_config.h         every tunable number, with its reason
│   ├── kasen_types.h          command, reply, alarm, recipe, cycle result
│   ├── sensing.h              load cells -> grams; tare; calibration; ADC backend
│   ├── logic.h                counts, recipe check, alarms, command handling
│   ├── comms.h                Wi-Fi + MQTT, topic names, JSON formats
│   └── ui.h                   LCD, buttons, LEDs
├── station/README_Station_Wiring.md   ACTUAL build: ADS1261 + INA333, pin map
└── demo/README_Demo_Wiring.md         TEST/DEMO build: 3 x HX711 + button simulation, pin map
```

You asked for `sensing.h` and `logic.h`. I added four small headers because those two cannot be written without them: the MQTT tags need a home (`comms.h`), logic has to call something to update the LCD (`ui.h`), and both share types and constants.

Still to come, one file per routine, in this order:

```
common/src/   sensing.c  logic_count.c  logic_cycle.c  logic_alarm.c
              logic_cmd.c  comms.c  comms_codec.c  ui.c  storage.c
station/      main.c  ads1261.c  CMakeLists.txt  prj.conf  app.overlay
demo/         main.c  hx711_sim.c  CMakeLists.txt  prj.conf  app.overlay
tests/        PC unit tests for the functions marked PURE
```

C has no classes, so "build by class" becomes "build by module": each header is one module, every function carries the module's prefix (`sensing_`, `logic_`, `comms_`, `ui_`), and each module keeps its data private in its `.c` file.

## How it all works

```
                 ┌──────────────────────── station ────────────────────────┐
 load cells ─► ADC backend ─► sensing ──readings──► logic ──► ui (LCD, LEDs)
 (x5)          ads1261 | demo   │  ▲                 │  ▲
                                │  └─tare/cal start──┘  │ button events
                                └─job events────────────┤
                                                        │ commands
                                             comms ◄────┘ counts, results,
                                               │          alarms, replies
                                               ▼
                              nRF7002 Wi-Fi ─► Pi access point ─► Mosquitto
```

Four threads, matching NLD tasks 3.1.3 to 3.1.6:

| Thread | Does | Talks to logic by |
|---|---|---|
| Sensing (sample intake) | Calls `sensing_process()` forever: reads 5 bins, filters, runs the tare or calibration job | `sensing_get_all()` plus job events |
| Logic (verification) | Runs Map 1. The only thread allowed to call `logic_` functions | owns the event queue |
| Comms (network) | Keeps Wi-Fi and MQTT up, decodes commands, sends or buffers | command and link events |
| UI (display) | Redraws the LCD from the latest `ui_model`, debounces the six buttons | button events |

Map 1 is drawn as one loop that polls four things. In the code the loop is still there (`logic_tick()`), but the hub message and the button arrive as queued events instead of being polled, so a slow LCD redraw or a Wi-Fi retry cannot delay a weight reading. The pseudo-code for the loop is at the top of `logic.h`.

### Where each process map lives

| Map | Function(s) |
|---|---|
| 1 Station main loop | `logic_tick()`, `sensing_process()`, `comms_start()` |
| 2 Tare | `sensing_tare_start()` then `logic_on_sensing_event()` |
| 3 Calibration | `sensing_calibrate_start()` then `logic_on_sensing_event()` |
| 4 Count | `logic_count_learn_unit_weight()`, `logic_count_from_weight()`, `logic_count_update()` |
| 5 Receive | `comms_decode_command()`, `logic_cmd_validate()`, `logic_on_command()` |
| 6 Send Count | `logic_count_publish_due()`, `comms_publish_count()` |
| 7 Recipe Check | `logic_cycle_request_start()`, `logic_cycle_request_complete()`, `logic_cycle_poll()`, `logic_cycle_evaluate()` |
| 8 Alarm | `logic_alarm_raise()`, `logic_alarm_clear()`, `logic_stock_check()` |
| 9, 10 Hub tabs | Pi side. The station's half is the MQTT contract below |

### Names in your repo and where they went

| Repo draft (`src/`) | Here |
|---|---|
| `NUM_BINS` | `KASEN_NUM_BINS` |
| `hardware_init()` | `sensing_init()`, `ui_init()`, `comms_init()` |
| `settings_load()` | inside `sensing_init()` and `logic_init()` |
| `network_start()`, `network_link_up()` | `comms_start()`, `comms_is_online()` |
| `read_load_cells()` | `sensing_process()`, `sensing_get_all()` |
| `counts_update()` | `logic_count_update()` |
| `counts_changed_or_2s_passed()` | `logic_count_publish_due()` |
| `counts_get()`, `counts_low_level()` | `logic_bin_get()` |
| `send_counts()`, `struct count_msg` | `comms_publish_count()`, `struct kasen_count_msg` |
| `hub_message_pending()`, `receive_handle_message()` | `logic_on_command()` |
| `button_pressed()`, `recipe_check_run()` | `logic_on_button()`, `logic_cycle_*()` |
| `any_bin_below_low_level()`, `alarm_run()` | `logic_stock_check()`, `logic_alarm_raise()` |

The maths in your `Counts.c` (`grams = (raw - tare) * g_per_count`, round to a count) is kept. The only change is that the factor is stored as counts per gram, as Map 3 words it.

## MQTT tags for the Pi developer

Full JSON examples are in the comment block at the top of `comms.h`. Summary (`<sid>` is the station ID, `<n>` is bin 1 to 5):

| Direction | Topic | Purpose | Map |
|---|---|---|---|
| station → hub | `kasen/<sid>/status` (retained, last-will) | online / offline | 1 |
| station → hub | `kasen/<sid>/bin/<n>/count` | part, count, weight, flags | 6 |
| station → hub | `kasen/<sid>/event/cycle` | pass or fail with per-bin expected and used | 7 |
| station → hub | `kasen/<sid>/event/alarm` | alarm raised or cleared | 8 |
| station → hub | `kasen/<sid>/reply` | done / failed / in_progress | 2, 3, 5 |
| hub → station | `kasen/<sid>/cmd/tare` | `{"req","bin"}` | 2 |
| hub → station | `kasen/<sid>/cmd/calibrate` | `{"req","bin","known_mg"}` | 3 |
| hub → station | `kasen/<sid>/cmd/part` | `{"req","bin","part","low_qty","unit_ug"}` | 5 |
| hub → station | `kasen/<sid>/cmd/sample` | `{"req","bin","qty"}`: "there are qty parts in this bin now" | 4 |
| hub → station | `kasen/<sid>/cmd/recipe` | `{"req","name","tol","low_kits","items":[{"bin","qty"}]}` | 7 |
| hub → station | `kasen/<sid>/cmd/alarm_clear` | `{"req","type","bin"}` | 8 |

The station connects the way Nordic's `nrf/samples/net/mqtt` sample does. That sample has a ready build configuration for this exact pair of boards (nRF54LM20 DK with shield `nrf7002eb2`), and it uses Nordic's `mqtt_helper` library over a plain TCP socket to port 1883. Your `wifi/` folder already covers the Wi-Fi half.

## Assumptions I made

Tell me which are wrong; each one is a small change now and a large one later.

1. **Language and RTOS:** C on Zephyr (nRF Connect SDK), as in your repo. Board target `nrf54lm20dk/nrf54lm20a/cpuapp`.
2. **Bin numbers:** 1 to 5 on the LCD and in MQTT, 0 to 4 inside C. Only the codec and the UI convert.
3. **Units:** float grams inside the firmware; integer milligrams and micrograms in MQTT.
4. **"Starting quantity" in the scope is the same thing as "sample quantity" in Map 4.** The user says how many parts are in the bin, the station divides the weight by that number. The hub may instead send a known unit weight and skip the sample.
5. **The station checks a recipe for its own 5 bins only.** A recipe for a merged group is checked by the hub from the count messages, as Map 9 says ("Hub keeps it").
6. **The station remembers its setup across power cycles:** tare, calibration, part names, unit weights, low levels, recipe. Map 3 asks for this for calibration; I extended it to the rest.
7. **No TLS** between station and Pi, since the Pi is a private access point.
8. **Station ID** is a build-time string (`"station1"`) for now.
9. **JSON payloads.** Easy to read in `mosquitto_sub` and easy for the Pi side.
10. **Time:** the station has no clock. Messages carry an age instead of a timestamp and the hub stamps them on arrival.
11. **Map 1 labels:** the box "Call Count Routine (Map 6)" names the Count routine but points at Map 6, which is Send Count. I read it as: counts are recomputed every loop (Map 4 maths) and sent by Map 6.

## Decisions you have made

| Question | Your answer | Effect |
|---|---|---|
| Pins on the DK | Use the FBD pins; ignore the DK's buttons and flash | One pin map for both builds, in `station/README_Station_Wiring.md`. Buttons are the FBD's BTN1 to BTN6 |
| Wi-Fi board | nRF7002 EB II | Build with shield `nrf7002eb2` |
| Demo rig | Three HX711s; the final unit follows the FBD | Bins 1 to 3 real, bins 4 and 5 simulated. The HX711s share one clock line |

Running on a DK with the FBD pins has three catches (flash on the port 2 pins, debugger serial port on P0.06 to P0.09, DK buttons on three signal lines). They are listed at the end of `station/README_Station_Wiring.md`.

## Things I found that you should look at

### 1. Calibration cannot answer inside the hub's 5 seconds

Map 9 shows "Failed" if the station has not replied in 5 s. Map 3 waits for a person to place a weight. I added an immediate `in_progress` reply; the final `done` or `failed` follows within 30 s. The hub needs to accept that for calibrate.

### 2. Map 5 has no "clear alarm" command, but Map 8 needs one

Map 8 has the hub telling the station to remove the alarm. I added `cmd/alarm_clear`.

### 3. One alarm flag is not enough

Map 8 asks "Alarm already active?" once. Taken literally, a low-stock alarm on bin 1 would hide a recipe mismatch on bin 3. Alarms are keyed by type and bin, so the question is asked per alarm.

### 4. A cleared low-stock alarm would come straight back

Map 1 raises the alarm on every loop while the bin is low. After the user clears it, the bin is still low. The station now remembers the acknowledgement until the bin has been restocked above its level.

### 5. Button presses with a hand still in the bin

If the count snapshot is taken the instant Start or Complete is pressed, a hand or a part still settling is weighed as stock. The cycle waits up to 1.5 s for steady readings before each snapshot. A bin that never settles is reported as "unverified" and the cycle does not pass.

### 6. Map 7 only reports failures

The pass branch sends a message; the fail branch only raises an alarm. I send a cycle result for both, so the hub has a record of every assembly with expected and used per bin.

### 7. Offline buffer

A buffer of 200 messages filled with a heartbeat from 5 bins every 2 s lasts 80 s and then starts dropping. While offline I keep only the newest count per bin, and keep every cycle result, alarm and reply.

### 8. Analog front end (hardware, not blocking the firmware)

These are from datasheet figures as I remember them; please check against the datasheets before acting.

- **INA333 noise against your lightest part.** A 1 kg, 1 mV/V cell at 5 V gives about 5 µV per gram. A 1/8 W resistor weighs roughly 0.1 g, so one part is roughly 0.5 µV. The INA333's low-frequency noise is listed at about 1 µV peak-to-peak, which is in the range of one to two of those parts and does not average away well. The ±1 part goal on the smallest resistors is at risk.
- **INA333 output near ground.** With REF tied to ground on a single 5 V supply, the output cannot go below a few tens of millivolts. A cell whose zero offset happens to be negative will sit on that floor and small weights will read as nothing. A small positive REF voltage avoids it.
- **A simpler option.** The ADS1261 has ten inputs (five differential pairs), a built-in amplifier up to gain 128, and can use the excitation voltage as its reference. Five load cells could connect to it directly with no INA333s. That removes the amplifier's noise and drift and makes the reading independent of the 5 V rail. If the course requires you to design the amplifier stage, ignore this.
- **Shared SPI bus.** The ADC and the LCD share SCK and MOSI. A full-screen redraw is about 100 kB and blocks ADC reads while it runs. The UI redraws only what changed to keep this short; a separate bus for the display would remove the issue.
- **Update rate.** One multiplexed ADC at 10 readings per second per bin needs 50 settled conversions per second. Faster conversions are noisier. If counts are jumpy, lower `KASEN_SENSE_RATE_HZ` first.

## Open questions

1. The FBD shows six buttons. The scope defines two (Start, Complete). What are BTN3 to BTN6 for on the real station? The demo uses them for the simulator.
2. What IP address will the Pi's access point use, and what SSID and password?
3. Which nRF Connect SDK version is on your machines? I checked against the current upstream sources.
4. Does the Pi developer agree to JSON and to the topic layout above?
5. How should a station get its ID when there are three of them: set at build time, or chosen on the hub?
6. Is a recipe tolerance other than 0 ever wanted?
7. The FBD has no status LEDs. Do you want any on the PCB (link, alarm)? The firmware uses the DK's LEDs when they exist and does nothing otherwise.

## Suggested order of work

1. Agree the pin map and the MQTT contract (this file, `comms.h`).
2. PURE functions and their PC tests: `sensing_average`, `sensing_is_steady`, `logic_count_from_weight`, `logic_cycle_evaluate`, `logic_kits_remaining`, `logic_cmd_validate`, the JSON codec.
3. Demo build with simulated bins only: proves logic, UI and MQTT with no analog hardware.
4. Add the three HX711s to the demo.
5. Station build with the ADS1261.
