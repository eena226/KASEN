/**
 * @file kasen_config.h
 * @brief Compile-time tunables shared by the station build and the demo build.
 *
 * Every number the firmware depends on lives here, with the reason it has
 * that value. Nothing in this file touches hardware. Pin assignments are NOT
 * here: they live in each build's devicetree overlay (see station/README.md
 * and demo/README.md).
 *
 * All values can be overridden from the build system (-DKASEN_x=y) because
 * each one is wrapped in #ifndef. When the project gets a Kconfig file these
 * should move there.
 */

#ifndef KASEN_CONFIG_H_
#define KASEN_CONFIG_H_

/* ------------------------------------------------------------------------ */
/* Station geometry                                                          */
/* ------------------------------------------------------------------------ */

/** Bin positions per station (Scope, Deliverable #1). */
#ifndef KASEN_NUM_BINS
#define KASEN_NUM_BINS 5
#endif

/** Usable weighing range after tare, in grams (Scope: 0.01 g to 500 g). */
#ifndef KASEN_RANGE_MAX_G
#define KASEN_RANGE_MAX_G 500.0f
#endif

/* ------------------------------------------------------------------------ */
/* Sensing (Maps 1, 2, 3)                                                    */
/* ------------------------------------------------------------------------ */

/**
 * Target refresh rate of every bin, in Hz. Taken from the "at least 10 reads
 * per second" comment in the repo's src/main.c. With one multiplexed ADC this
 * means 5x this many conversions per second, which costs noise. Lower it if
 * the +/-1 part goal is not met (see README, "Things to check").
 */
#ifndef KASEN_SENSE_RATE_HZ
#define KASEN_SENSE_RATE_HZ 10
#endif

/** Moving-average length applied to raw ADC codes before anything uses them. */
#ifndef KASEN_FILTER_WINDOW
#define KASEN_FILTER_WINDOW 8
#endif

/** Number of most recent filtered samples compared to decide "steady". */
#ifndef KASEN_STABLE_WINDOW
#define KASEN_STABLE_WINDOW 10
#endif

/**
 * A bin is "steady" when the peak-to-peak spread of the last
 * KASEN_STABLE_WINDOW samples is at most this many raw ADC codes.
 *
 * It is in raw codes, not grams, because tare and calibration must judge
 * steadiness before a grams scale exists. Both ADCs land near 2100 codes per
 * gram with a 1 kg, 1 mV/V cell at 5 V, so 100 codes is about 0.05 g, under
 * half the weight of the lightest supported part. Re-tune on real hardware.
 */
#ifndef KASEN_STABLE_BAND_RAW
#define KASEN_STABLE_BAND_RAW 100
#endif

/** Map 2: "Read the bin 10 times; average the raw count". */
#ifndef KASEN_TARE_SAMPLES
#define KASEN_TARE_SAMPLES 10
#endif

/** Map 2: "Retry > 3 times?" then fail. */
#ifndef KASEN_TARE_MAX_RETRIES
#define KASEN_TARE_MAX_RETRIES 3
#endif

/** Map 3: samples averaged once the known weight is on the bin. */
#ifndef KASEN_CAL_SAMPLES
#define KASEN_CAL_SAMPLES 10
#endif

/**
 * Map 3: how long the station waits for the user to place the known weight
 * before it replies 'Failed'. Not on the flowchart; added so a forgotten
 * calibration cannot hang the station.
 */
#ifndef KASEN_CAL_WAIT_TIMEOUT_MS
#define KASEN_CAL_WAIT_TIMEOUT_MS 30000
#endif

/**
 * Map 3: "Steady and above empty reading?". The loaded reading must exceed
 * the tare reading by at least this many raw codes (about 2.5 g) or the
 * station assumes no weight was placed.
 */
#ifndef KASEN_CAL_MIN_DELTA_RAW
#define KASEN_CAL_MIN_DELTA_RAW 5000
#endif

/** Smallest known weight accepted for calibration, in grams. */
#ifndef KASEN_CAL_MIN_KNOWN_G
#define KASEN_CAL_MIN_KNOWN_G 10.0f
#endif

/* ------------------------------------------------------------------------ */
/* Counting (Map 4, Map 1 decision)                                          */
/* ------------------------------------------------------------------------ */

/**
 * Smallest net sample weight accepted when learning a part's unit weight.
 * A sample lighter than this is mostly noise, so the unit weight (and every
 * count after it) would be wrong. Not on the flowchart; added.
 */
#ifndef KASEN_SAMPLE_MIN_NET_G
#define KASEN_SAMPLE_MIN_NET_G 1.0f
#endif

/**
 * Count hysteresis, as a fraction of one unit weight. The displayed count
 * only changes when the weight is this far past the rounding boundary, so a
 * reading sitting at x.5 parts does not flicker between two counts.
 */
#ifndef KASEN_COUNT_HYSTERESIS
#define KASEN_COUNT_HYSTERESIS 0.15f
#endif

/** Map 1: "Any count changed, or have 2 sec passed?". */
#ifndef KASEN_HEARTBEAT_MS
#define KASEN_HEARTBEAT_MS 2000
#endif

/** Low-stock threshold used until the hub sends one (repo: DEFAULT_LOW_LEVEL). */
#ifndef KASEN_DEFAULT_LOW_QTY
#define KASEN_DEFAULT_LOW_QTY 5
#endif

/* ------------------------------------------------------------------------ */
/* Assembly cycle (Map 7)                                                    */
/* ------------------------------------------------------------------------ */

/**
 * After Start Assembly or Assembly Complete is pressed, wait up to this long
 * for every bin to be steady before taking the snapshot. A hand still in a
 * bin would otherwise be weighed as parts. Not on the flowchart; added.
 */
#ifndef KASEN_CYCLE_SETTLE_TIMEOUT_MS
#define KASEN_CYCLE_SETTLE_TIMEOUT_MS 1500
#endif

/* ------------------------------------------------------------------------ */
/* Strings and buffers                                                       */
/* ------------------------------------------------------------------------ */

/** Sizes include the terminating NUL. Values match the repo's mqtt.txt draft. */
#ifndef KASEN_STATION_ID_LEN
#define KASEN_STATION_ID_LEN 12
#endif

#ifndef KASEN_PART_NAME_LEN
#define KASEN_PART_NAME_LEN 24
#endif

#ifndef KASEN_RECIPE_NAME_LEN
#define KASEN_RECIPE_NAME_LEN 24
#endif

/** Messages kept in RAM while the link is down (repo: BUF_MAX_MSGS). */
#ifndef KASEN_TX_BUFFER_MSGS
#define KASEN_TX_BUFFER_MSGS 200
#endif

/* ------------------------------------------------------------------------ */
/* Network (Maps 5, 6, 9)                                                    */
/* ------------------------------------------------------------------------ */

/** First level of every MQTT topic. */
#ifndef KASEN_MQTT_TOPIC_ROOT
#define KASEN_MQTT_TOPIC_ROOT "kasen"
#endif

/** Station name used in topics until it is set per board. */
#ifndef KASEN_DEFAULT_STATION_ID
#define KASEN_DEFAULT_STATION_ID "station1"
#endif

/** Mosquitto on the Pi, plain MQTT (no TLS on the Pi's private access point). */
#ifndef KASEN_MQTT_BROKER_PORT
#define KASEN_MQTT_BROKER_PORT 1883
#endif

/**
 * Map 9: the hub shows 'Failed' if a station has not replied within this
 * time. The station does not use this value; it is here so both sides agree.
 * Calibration takes longer than this by design, see comms.h.
 */
#ifndef KASEN_HUB_REPLY_TIMEOUT_MS
#define KASEN_HUB_REPLY_TIMEOUT_MS 5000
#endif

#endif /* KASEN_CONFIG_H_ */
