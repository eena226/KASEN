/**
 * @file sensing.h
 * @brief Everything between the load cells and a weight in grams.
 *
 * Process maps covered:
 *  - Map 1  "Read all 5 load cells"
 *  - Map 2  Tare routine
 *  - Map 3  Calibration routine
 *
 * What this module does NOT know about: parts, counts, recipes, MQTT, the
 * LCD. It turns ADC codes into grams and reports when a tare or calibration
 * finishes. logic.h builds counts on top of it.
 *
 * Signal path (one per bin):
 *
 *   load cell -> amplifier -> ADC -> raw code -> moving average
 *             -> (raw - tare_raw) / counts_per_g -> net grams
 *
 * Station build vs demo build
 * ---------------------------
 * The two builds differ only in the ADC. That difference is hidden behind
 * struct sensing_backend (bottom of this file):
 *
 *   station/  sensing_backend_ads1261   ADS1261, 5 inputs, INA333 per cell
 *   demo/     sensing_backend_demo      three HX711s on bins 1 to 3,
 *                                       simulated load cells on bins 4, 5
 *
 * Everything above the backend (filtering, tare, calibration, storage) is
 * the same source file in both builds, so the demo exercises the real code.
 *
 * Threading
 * ---------
 * sensing_process() is called repeatedly from one thread (the sensing
 * thread). Every other function may be called from any thread. Tare and
 * calibration are "jobs": the start function returns at once, the sensing
 * thread does the work across later sensing_process() calls, and the result
 * arrives through the event callback. This is what lets the station keep
 * weighing the other bins while one bin is being calibrated (Map 1 never
 * stops looping).
 *
 * Storage
 * -------
 * Tare offset and calibration factor are saved per bin to non-volatile
 * storage on success (Map 3 "Save in memory in Non-Volatile Storage") and
 * loaded by sensing_init().
 */

#ifndef KASEN_SENSING_H_
#define KASEN_SENSING_H_

#include "kasen_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Readings                                                                  */
/* ------------------------------------------------------------------------ */

/** Bit flags in sensing_reading.flags. */
#define SENSING_FLAG_TARED      (1u << 0) /**< A zero offset is stored. */
#define SENSING_FLAG_CALIBRATED (1u << 1) /**< A codes-per-gram factor is stored. */
#define SENSING_FLAG_STABLE     (1u << 2) /**< Steady, see KASEN_STABLE_BAND_RAW. */
#define SENSING_FLAG_OVERLOAD   (1u << 3) /**< Above KASEN_RANGE_MAX_G or ADC clipped. */
#define SENSING_FLAG_FAULT      (1u << 4) /**< ADC read failed. */
#define SENSING_FLAG_SIMULATED  (1u << 5) /**< Demo: value comes from the simulator. */

/** Latest state of one bin's load cell. */
struct sensing_reading {
	/** Filtered ADC code. Meaningful even before tare and calibration. */
	int32_t raw;
	/**
	 * Weight above the tare point, grams. 0 until the bin is both tared
	 * and calibrated. May be slightly negative on an empty bin.
	 */
	float net_g;
	/** SENSING_FLAG_x bits. */
	uint8_t flags;
	/** Uptime of the newest sample in this reading. */
	int64_t timestamp_ms;
};

/**
 * True if net_g can be trusted for counting: tared, calibrated, no fault,
 * no overload. Steadiness is separate; test SENSING_FLAG_STABLE for that.
 */
static inline bool sensing_reading_usable(const struct sensing_reading *r)
{
	const uint8_t need = SENSING_FLAG_TARED | SENSING_FLAG_CALIBRATED;
	const uint8_t bad = SENSING_FLAG_OVERLOAD | SENSING_FLAG_FAULT;

	return (r != NULL) && ((r->flags & need) == need) &&
	       ((r->flags & bad) == 0u);
}

/** Per-bin values that turn a raw code into grams. Saved to flash. */
struct sensing_cal {
	int32_t tare_raw;    /**< Map 2 "zero offset": raw code of the empty bin. */
	float counts_per_g;  /**< Map 3 "counts per gram": raw codes per gram. */
	bool tared;          /**< tare_raw is valid. */
	bool calibrated;     /**< counts_per_g is valid. */
};

/* ------------------------------------------------------------------------ */
/* Events (results of tare and calibration jobs)                             */
/* ------------------------------------------------------------------------ */

enum sensing_event_type {
	SENSING_EVT_TARE_DONE,    /**< Map 2 "Save the average as the zero offset". */
	SENSING_EVT_TARE_FAILED,  /**< Map 2 either 'Failed' exit. */
	/**
	 * Map 3 "Prompt user to place the known weight". The LCD should show
	 * the prompt now.
	 */
	SENSING_EVT_CAL_WAITING,
	SENSING_EVT_CAL_DONE,     /**< Map 3 "Save in memory" reached. */
	SENSING_EVT_CAL_FAILED,   /**< Map 3 'Failed' exit, or timeout. */
	SENSING_EVT_FAULT,        /**< A bin's ADC channel stopped giving data. */
	SENSING_EVT_FAULT_CLEARED /**< It recovered. */
};

struct sensing_event {
	enum sensing_event_type type;
	kasen_bin_t bin;
	uint32_t req_id;    /**< From the start call; 0 for FAULT events. */
	enum kasen_err err; /**< Reason for a FAILED or FAULT event. */
};

/**
 * Called from the sensing thread when a job finishes or a fault changes.
 * Must return quickly and must not call sensing_process(). The usual body
 * copies the event into the logic thread's queue.
 */
typedef void (*sensing_event_cb_t)(const struct sensing_event *evt);

/* ------------------------------------------------------------------------ */
/* Lifecycle and acquisition (Map 1)                                         */
/* ------------------------------------------------------------------------ */

/**
 * Start the ADC and load saved tare/calibration values.
 *
 * Operations:
 *  1. Remember @p cb.
 *  2. Call the backend's init().
 *  3. Load struct sensing_cal for each bin from non-volatile storage; a bin
 *     with nothing stored starts untared and uncalibrated.
 *  4. Clear filters and mark every reading as not stable.
 *
 * @param cb Event callback; may be NULL if no one wants events.
 * @retval 0                  Ready.
 * @retval -KASEN_ERR_SENSOR  The ADC did not respond.
 * @retval -KASEN_ERR_STORAGE Stored values were unreadable (defaults used;
 *                            sensing still runs).
 */
int sensing_init(sensing_event_cb_t cb);

/**
 * One acquisition pass. Map 1: "Read all 5 load cells".
 *
 * Call in a loop from the sensing thread. Blocks until every bin has one
 * new sample (about 1 / KASEN_SENSE_RATE_HZ seconds).
 *
 * Operations, for each bin:
 *  1. backend->read_raw(). On error set SENSING_FLAG_FAULT and go to the
 *     next bin; raise SENSING_EVT_FAULT once, not on every pass.
 *  2. Push the code into the bin's moving average (KASEN_FILTER_WINDOW).
 *  3. Update SENSING_FLAG_STABLE from the last KASEN_STABLE_WINDOW values.
 *  4. Compute net_g if tared and calibrated; set SENSING_FLAG_OVERLOAD if
 *     above KASEN_RANGE_MAX_G.
 *  5. Store the reading where sensing_get() can copy it.
 * Then, once per pass:
 *  6. Advance the running tare/calibration job, if any, by one step.
 *
 * @retval 0                 All bins read.
 * @retval -KASEN_ERR_SENSOR At least one bin failed (others are still valid).
 */
int sensing_process(void);

/**
 * Copy the latest reading of one bin.
 *
 * @param bin Bin index.
 * @param[out] out Receives the reading.
 * @retval 0                   OK.
 * @retval -KASEN_ERR_BAD_BIN   @p bin out of range.
 * @retval -KASEN_ERR_BAD_VALUE @p out is NULL.
 */
int sensing_get(kasen_bin_t bin, struct sensing_reading *out);

/**
 * Copy the latest readings of all bins from the same acquisition pass.
 * Use this, not five sensing_get() calls, wherever bins are compared with
 * each other (recipe check).
 *
 * @param[out] out Array of KASEN_NUM_BINS readings.
 * @retval 0                   OK.
 * @retval -KASEN_ERR_BAD_VALUE @p out is NULL.
 */
int sensing_get_all(struct sensing_reading out[KASEN_NUM_BINS]);

/* ------------------------------------------------------------------------ */
/* Tare routine (Map 2)                                                      */
/* ------------------------------------------------------------------------ */

/**
 * Begin taring one bin. The bin must be empty and in place.
 *
 * Returns at once. The sensing thread then follows Map 2:
 *  1. Collect KASEN_TARE_SAMPLES new samples and average them.
 *  2. "Reading steady?" (spread within KASEN_STABLE_BAND_RAW)
 *       no  -> retry from 1, up to KASEN_TARE_MAX_RETRIES times, then
 *              SENSING_EVT_TARE_FAILED with KASEN_ERR_UNSTABLE.
 *       yes -> continue.
 *  3. Save the average as tare_raw, set tared, write to storage.
 *  4. SENSING_EVT_TARE_DONE.
 *
 * The caller (logic) turns the event into the LCD update and the MQTT
 * 'Done'/'Failed' reply shown on the map.
 *
 * Taring does not change counts_per_g. Taring a bin that already has parts
 * in it makes those parts invisible; logic should warn, not prevent.
 *
 * @param bin    Bin index. Map 2 "Bin number 1 to 5?" is checked here.
 * @param req_id Returned in the event.
 * @retval 0                 Job started.
 * @retval -KASEN_ERR_BAD_BIN @p bin out of range (Map 2 first 'Failed').
 * @retval -KASEN_ERR_BUSY    Another job is running.
 * @retval -KASEN_ERR_SENSOR  The bin's channel is in fault.
 */
int sensing_tare_start(kasen_bin_t bin, uint32_t req_id);

/* ------------------------------------------------------------------------ */
/* Calibration routine (Map 3)                                               */
/* ------------------------------------------------------------------------ */

/**
 * Begin calibrating one bin with a known weight.
 *
 * Returns at once. The sensing thread then follows Map 3:
 *  1. "Bin tared?" no -> run the tare steps of Map 2 first (the bin must be
 *     empty at this moment). If that tare fails, SENSING_EVT_CAL_FAILED.
 *  2. SENSING_EVT_CAL_WAITING ("Prompt user to place the known weight").
 *  3. Wait until the reading is steady AND at least KASEN_CAL_MIN_DELTA_RAW
 *     above tare_raw ("Steady and above empty reading?"). Give up after
 *     KASEN_CAL_WAIT_TIMEOUT_MS: SENSING_EVT_CAL_FAILED with
 *     KASEN_ERR_TIMEOUT if nothing was placed, KASEN_ERR_UNSTABLE if it
 *     never settled.
 *  4. Average KASEN_CAL_SAMPLES samples ("Read and average the bin").
 *  5. counts_per_g = (average - tare_raw) / known_g.
 *  6. Save to storage, set calibrated, SENSING_EVT_CAL_DONE.
 *
 * Step 3 replaces a "user presses OK" step: the station notices the weight
 * arriving by itself, so calibration needs no extra button.
 *
 * @param bin     Bin index.
 * @param known_g Mass of the reference weight, grams. Must be between
 *                KASEN_CAL_MIN_KNOWN_G and KASEN_RANGE_MAX_G.
 * @param req_id  Returned in the events.
 * @retval 0                   Job started.
 * @retval -KASEN_ERR_BAD_BIN   @p bin out of range.
 * @retval -KASEN_ERR_BAD_VALUE @p known_g out of range.
 * @retval -KASEN_ERR_BUSY      Another job is running.
 * @retval -KASEN_ERR_SENSOR    The bin's channel is in fault.
 */
int sensing_calibrate_start(kasen_bin_t bin, float known_g, uint32_t req_id);

/** True while a tare or calibration job is running. */
bool sensing_busy(void);

/**
 * Stop the running job, if any, without changing stored values. No event is
 * raised. Used when the link drops mid-calibration or the user cancels.
 */
void sensing_job_cancel(void);

/* ------------------------------------------------------------------------ */
/* Calibration data access                                                   */
/* ------------------------------------------------------------------------ */

/**
 * Copy a bin's current tare/calibration values.
 *
 * @retval 0 OK. @retval -KASEN_ERR_BAD_BIN / -KASEN_ERR_BAD_VALUE as above.
 */
int sensing_cal_get(kasen_bin_t bin, struct sensing_cal *out);

/**
 * Replace a bin's tare/calibration values and save them. For restoring a
 * backup or entering a known factor without a reference weight.
 *
 * @retval 0                   Applied and saved.
 * @retval -KASEN_ERR_BAD_BIN   @p bin out of range.
 * @retval -KASEN_ERR_BAD_VALUE NULL, or calibrated with counts_per_g <= 0.
 * @retval -KASEN_ERR_BUSY      A job is running on this bin.
 * @retval -KASEN_ERR_STORAGE   Applied in RAM but not saved.
 */
int sensing_cal_set(kasen_bin_t bin, const struct sensing_cal *cal);

/* ------------------------------------------------------------------------ */
/* Pure helpers (no hardware, no state; unit-test these on a PC)             */
/* ------------------------------------------------------------------------ */

/**
 * Integer mean of @p n raw codes. Map 2 "average the raw count".
 * @return The mean, or 0 if @p n is 0 or @p samples is NULL.
 */
int32_t sensing_average(const int32_t *samples, size_t n);

/**
 * "Reading steady?" (Maps 2, 3): true if max - min of the samples is at
 * most @p band_raw. False if @p n < 2 or @p samples is NULL.
 */
bool sensing_is_steady(const int32_t *samples, size_t n, int32_t band_raw);

/**
 * Map 3 "Calculate counts per gram".
 * @return (loaded_raw - tare_raw) / known_g, or 0 if known_g <= 0 or the
 *         result would not be positive.
 */
float sensing_compute_counts_per_g(int32_t loaded_raw, int32_t tare_raw,
				   float known_g);

/**
 * Raw code to net grams using a bin's calibration.
 * @return (raw - tare_raw) / counts_per_g, or 0 if @p cal is NULL, not
 *         tared, or not calibrated.
 */
float sensing_raw_to_grams(int32_t raw, const struct sensing_cal *cal);

/* ------------------------------------------------------------------------ */
/* ADC backend interface                                                     */
/* ------------------------------------------------------------------------ */

/**
 * The only hardware-specific part of sensing. One instance per ADC type.
 *
 * To support another ADC, write these two functions and nothing else in the
 * firmware changes.
 */
struct sensing_backend {
	/** Name for logs, e.g. "ads1261". */
	const char *name;

	/**
	 * Power up, reset and configure the ADC.
	 * @retval 0 OK. @retval -KASEN_ERR_SENSOR No response.
	 */
	int (*init)(void);

	/**
	 * Return one settled conversion for one bin. Blocks until it is
	 * ready (waits for DRDY). For a multiplexed ADC this selects the
	 * bin's input and discards whatever is needed for the filter to
	 * settle, so the value never contains a previous bin's signal.
	 *
	 * @param bin Bin index, already range-checked by the caller.
	 * @param[out] raw Signed code; larger = heavier.
	 * @retval 0 OK.
	 * @retval -KASEN_ERR_SENSOR  SPI error or implausible data.
	 * @retval -KASEN_ERR_TIMEOUT DRDY never came.
	 */
	int (*read_raw)(kasen_bin_t bin, int32_t *raw);
};

/**
 * Station build. ADS1261 on SPI.
 *
 * Wiring per the FBD: bin 1..5 on AIN0..AIN4, each measured against AINCOM
 * (ground); reference = AVDD (5 V); internal clock; PGA gain 1 because the
 * INA333 already provides the gain of about 250.
 *
 * Implementation notes for ads1261.c:
 *  - SPI mode 1 (CPOL 0, CPHA 1).
 *  - init: pulse RESET, wait, write MODE/REF/PGA registers, read them back
 *    to prove the chip is there.
 *  - read_raw: write INPMUX for the bin, send START, wait for the DRDY
 *    interrupt, send RDATA, sign-extend the 24-bit result.
 *  - TI publishes a reference C driver (ads1261.c/.h) that can be adapted.
 */
extern const struct sensing_backend sensing_backend_ads1261;

/**
 * Demo build. Bins 1 to 3 each have a real HX711; bins 4 and 5 read the
 * simulator below. Which bins are real is a devicetree choice in demo/.
 *
 * Implementation notes for the HX711 part (hx711_sim.c):
 *  - The three HX711s share one clock line (PD_SCK) and have one data line
 *    (DOUT) each: four GPIOs in total. See demo/README_Demo_Wiring.md.
 *  - A conversion is ready when a chip pulls its DOUT low, and the chip
 *    holds it until it is clocked out. Wait until all three are low.
 *  - Then send 25 clock pulses; after each rising edge read all three DOUT
 *    pins. Bits 1..24 are the result, MSB first, two's complement. The
 *    25th pulse selects channel A, gain 128 for the next conversion.
 *  - PD_SCK must never stay high longer than about 50 us or the chips
 *    power down. Keep interrupts locked while the line is high.
 *  - Do the burst once per sensing_process() pass and cache the three
 *    values; read_raw() for bins 1 to 3 returns the cached value.
 *  - A chip whose DOUT never goes low within 2 conversion periods is in
 *    fault (-KASEN_ERR_TIMEOUT for that bin only).
 *  - Zephyr also has an SPI-based HX711 driver, but it needs one SPI port
 *    per chip. With three chips the shared clock is fewer wires.
 */
extern const struct sensing_backend sensing_backend_demo;

/**
 * The backend this build was compiled with. sensing.c calls only this.
 */
const struct sensing_backend *sensing_backend_get(void);

/* ------------------------------------------------------------------------ */
/* Load-cell simulator (demo build only)                                     */
/* ------------------------------------------------------------------------ */

/*
 * The simulator fakes the raw ADC code of a load cell, not the count. A
 * simulated bin therefore still has to be tared, calibrated and given a
 * sample quantity exactly like a real one, which is the point of the demo.
 *
 *   raw = sim_zero + sim_counts_per_g * (bin_g + parts * part_g) + noise
 *
 * BTN3 to BTN6 call these functions (see demo/README_Demo_Wiring.md for the mapping).
 * In the station build they are not compiled in.
 */

/**
 * Set the weight of one simulated part for a bin. Default KASEN_SIM_PART_G.
 * @retval 0 OK. @retval -KASEN_ERR_BAD_BIN / -KASEN_ERR_BAD_VALUE (<= 0).
 */
int sensing_sim_set_part_weight(kasen_bin_t bin, float part_g);

/**
 * Add (positive) or remove (negative) simulated parts. The part total never
 * goes below 0.
 * @retval 0 OK. @retval -KASEN_ERR_BAD_BIN if the bin is out of range or is
 *         a real HX711 bin.
 */
int sensing_sim_add_parts(kasen_bin_t bin, int32_t parts);

/**
 * Add or remove loose weight in grams, for example to "place" and later
 * "remove" a calibration weight on a simulated bin.
 * @retval 0 OK. @retval -KASEN_ERR_BAD_BIN as above.
 */
int sensing_sim_add_grams(kasen_bin_t bin, float grams);

/**
 * Place the simulated reference weight (KASEN_SIM_REF_WEIGHT_G) on a bin if
 * it is not there, or take it off if it is. This is how a simulated bin is
 * calibrated: the hub sends calibrate with the same weight, the LCD prompts,
 * the user presses the reference-weight button, and presses it again after
 * 'Done'.
 * @retval 1 Weight is now on the bin. @retval 0 Weight is now off.
 * @retval -KASEN_ERR_BAD_BIN as above.
 */
int sensing_sim_toggle_ref_weight(kasen_bin_t bin);

#ifdef __cplusplus
}
#endif

#endif /* KASEN_SENSING_H_ */
