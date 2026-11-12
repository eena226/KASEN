/**
 * @file kasen_types.h
 * @brief Data types passed between the modules (sensing, logic, comms, ui).
 *
 * These are the "nouns" of the firmware: a command from the hub, a reply, a
 * count message, an alarm, a recipe, a cycle result. Each one appears on a
 * process map; the map is named next to the type.
 *
 * Conventions used in every header:
 *  - Bin numbers in C are 0-based (0..KASEN_NUM_BINS-1). On the LCD and in
 *    MQTT they are 1-based (1..5), as on the process maps. Only ui.c and the
 *    comms codec convert.
 *  - Weights in C are float grams. On the wire they are integers
 *    (milligrams, micrograms) so no side has to parse decimals.
 *  - Time is int64 milliseconds since boot (k_uptime_get()). The station has
 *    no real-time clock.
 *  - Functions return 0 on success or a negative enum kasen_err, for example
 *    -KASEN_ERR_UNSTABLE.
 *
 * This header has no Zephyr dependency so the logic can be unit-tested on a
 * PC.
 */

#ifndef KASEN_TYPES_H_
#define KASEN_TYPES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kasen_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Bins                                                                      */
/* ------------------------------------------------------------------------ */

/** 0-based bin index. */
typedef uint8_t kasen_bin_t;

/** Used where something is not tied to one bin (for example a low-kits alarm). */
#define KASEN_BIN_NONE ((kasen_bin_t)0xFF)

/** Map 2: "Bin number 1 to 5?" expressed for a 0-based index. */
#define KASEN_BIN_IS_VALID(bin) ((bin) < KASEN_NUM_BINS)

/** One bit per bin, bit 0 = first bin. */
typedef uint8_t kasen_bin_mask_t;

#define KASEN_BIN_BIT(bin) ((kasen_bin_mask_t)(1u << (bin)))
#define KASEN_BIN_MASK_ALL ((kasen_bin_mask_t)((1u << KASEN_NUM_BINS) - 1u))

/* ------------------------------------------------------------------------ */
/* Error codes                                                               */
/* ------------------------------------------------------------------------ */

/**
 * Why something failed. Returned negated from functions and carried (as is)
 * in replies to the hub, where it becomes the short text from
 * kasen_err_str().
 */
enum kasen_err {
	KASEN_OK = 0,
	KASEN_ERR_BAD_BIN,        /**< Bin number outside 1..5 (Map 2). */
	KASEN_ERR_BAD_VALUE,      /**< A value failed "Values valid?" (Maps 5, 9). */
	KASEN_ERR_UNKNOWN_CMD,    /**< Command not recognised (Map 5). */
	KASEN_ERR_UNSTABLE,       /**< Reading never became steady (Maps 2, 3). */
	KASEN_ERR_NO_WEIGHT,      /**< Not above the empty reading (Map 3). */
	KASEN_ERR_TOO_LIGHT,      /**< Sample too light to learn a unit weight. */
	KASEN_ERR_OVERLOAD,       /**< Above KASEN_RANGE_MAX_G. */
	KASEN_ERR_NOT_TARED,      /**< Bin has no zero offset yet. */
	KASEN_ERR_NOT_CALIBRATED, /**< Bin has no codes-per-gram yet. */
	KASEN_ERR_NO_UNIT_WEIGHT, /**< Bin has no part weight yet (Map 4). */
	KASEN_ERR_NO_RECIPE,      /**< Start Assembly with no recipe loaded. */
	KASEN_ERR_BAD_STATE,      /**< Wrong moment, e.g. Complete before Start. */
	KASEN_ERR_BUSY,           /**< Another tare/calibration is running. */
	KASEN_ERR_TIMEOUT,        /**< Gave up waiting (e.g. for the known weight). */
	KASEN_ERR_SENSOR,         /**< ADC did not answer or returned garbage. */
	KASEN_ERR_STORAGE,        /**< Could not save to non-volatile storage. */
	KASEN_ERR_COMMS,          /**< Could not send and could not buffer. */
	KASEN_ERR_NO_MEM,         /**< A fixed-size table or buffer is full. */

	KASEN_ERR_COUNT           /**< Number of codes. Keep last. */
};

/**
 * Short lowercase text for an error, used in MQTT replies and logs.
 *
 * @param err Error code, positive or negated.
 * @return Constant string such as "unstable"; "unknown" if out of range.
 */
const char *kasen_err_str(int err);

/* ------------------------------------------------------------------------ */
/* Recipe (Map 7, Map 9 "Upload a recipe")                                   */
/* ------------------------------------------------------------------------ */

/**
 * What one assembly is expected to consume from this station's bins.
 *
 * A recipe covers one station (5 bins). A recipe for a merged group of
 * stations stays on the hub (Map 9: "Hub keeps it").
 */
struct kasen_recipe {
	/** Human name shown on the LCD, e.g. "PCB-A rev2". */
	char name[KASEN_RECIPE_NAME_LEN];
	/**
	 * Parts taken from each bin per assembly. 0 means the bin is not part
	 * of this assembly, so any removal from it is flagged as "too many".
	 */
	uint16_t qty[KASEN_NUM_BINS];
	/** Allowed difference per bin between used and expected. Default 0. */
	uint8_t tolerance;
	/**
	 * Raise a low-kits alarm when this many complete assemblies or fewer
	 * can still be built (Scope, Deliverable #5). 0 disables it.
	 */
	uint16_t low_kits;
};

/* ------------------------------------------------------------------------ */
/* Commands from the hub (Map 5 "Which command?")                            */
/* ------------------------------------------------------------------------ */

enum kasen_cmd_type {
	KASEN_CMD_TARE,           /**< Map 5 "Tare (Map 2)". */
	KASEN_CMD_CALIBRATE,      /**< Map 5 "Calibrate (Map 3)". */
	KASEN_CMD_SET_PART,       /**< Map 5 "Set bin part information". */
	KASEN_CMD_SET_SAMPLE_QTY, /**< Map 5 "Set sample quantity (Map 4)". */
	KASEN_CMD_LOAD_RECIPE,    /**< Map 5 "Load recipe (Map 7)". */
	KASEN_CMD_CLEAR_ALARM,    /**< Map 8 "User clears the popup". Not listed
				   *   on Map 5 but Map 8 needs it. */

	KASEN_CMD_COUNT           /**< Number of commands. Keep last. */
};

/**
 * One decoded command. comms fills it in from an MQTT message; logic
 * validates it and runs the matching routine.
 */
struct kasen_cmd {
	enum kasen_cmd_type type;
	/**
	 * Number chosen by the hub and echoed in the reply so the hub can
	 * match replies to requests. 0 if the hub did not send one.
	 */
	uint32_t req_id;
	/** Target bin. KASEN_BIN_NONE for KASEN_CMD_LOAD_RECIPE. */
	kasen_bin_t bin;

	/** Extra values; which member is valid depends on @ref type. */
	union {
		/** KASEN_CMD_CALIBRATE */
		struct {
			float known_g; /**< Weight the user will place. */
		} calibrate;

		/** KASEN_CMD_SET_PART */
		struct {
			char part_name[KASEN_PART_NAME_LEN];
			/** Low-stock alarm level in parts. 0 disables it. */
			uint16_t low_qty;
			/**
			 * Weight of one part if the hub already knows it,
			 * else 0 and the station learns it from a sample
			 * (Map 4).
			 */
			float unit_weight_g;
		} part;

		/** KASEN_CMD_SET_SAMPLE_QTY */
		struct {
			/** How many parts are in the bin right now. */
			uint32_t qty;
		} sample;

		/** KASEN_CMD_LOAD_RECIPE */
		struct kasen_recipe recipe;

		/** KASEN_CMD_CLEAR_ALARM (bin comes from @ref bin) */
		struct {
			uint8_t alarm_type; /**< enum kasen_alarm_type */
		} clear_alarm;
	} u;
};

/* ------------------------------------------------------------------------ */
/* Replies to the hub (Maps 2, 3, 5: 'Done' / 'Failed')                      */
/* ------------------------------------------------------------------------ */

enum kasen_reply_status {
	KASEN_REPLY_DONE,        /**< 'Done' on the maps. */
	KASEN_REPLY_FAILED,      /**< 'Failed' on the maps; see err. */
	/**
	 * Accepted, final answer follows. Sent at once for calibration, which
	 * waits for a person and so cannot finish inside the hub's 5 s window
	 * (Map 9). Not on the maps; added.
	 */
	KASEN_REPLY_IN_PROGRESS
};

struct kasen_reply {
	uint32_t req_id;               /**< Copied from the command. */
	enum kasen_cmd_type cmd;       /**< Which command this answers. */
	kasen_bin_t bin;               /**< Copied from the command. */
	enum kasen_reply_status status;
	enum kasen_err err;            /**< KASEN_OK unless status is FAILED. */
};

/* ------------------------------------------------------------------------ */
/* Count message (Map 6 "Build message")                                     */
/* ------------------------------------------------------------------------ */

/**
 * State of one bin as sent to the hub and shown on the LCD.
 * Map 6 lists: station ID, bin number, part name, count, time. The station
 * ID is added by comms. The extra flags let the hub grey out a count that
 * cannot be trusted instead of showing a wrong number.
 */
struct kasen_count_msg {
	kasen_bin_t bin;
	char part_name[KASEN_PART_NAME_LEN];
	int32_t count;       /**< Estimated parts in the bin. */
	float net_g;         /**< Weight above tare, grams. */
	bool valid;          /**< false until tared, calibrated and unit weight known. */
	bool stable;         /**< Reading was steady when captured. */
	bool low;            /**< At or below the bin's low-stock level. */
	int64_t captured_ms; /**< Uptime when captured. */
};

/* ------------------------------------------------------------------------ */
/* Alarms (Map 8)                                                            */
/* ------------------------------------------------------------------------ */

enum kasen_alarm_type {
	KASEN_ALARM_LOW_STOCK,       /**< Map 8 start: "Bin low". */
	KASEN_ALARM_RECIPE_MISMATCH, /**< Map 8 start: "Recipe Check failed". */
	KASEN_ALARM_LOW_KITS,        /**< Too few complete assemblies left. */
	KASEN_ALARM_SENSOR_FAULT,    /**< ADC fault or overload on a bin. */

	KASEN_ALARM_TYPE_COUNT       /**< Keep last. */
};

/**
 * One alarm. An alarm is identified by (type, bin): the same pair cannot be
 * active twice (Map 8 "Alarm already active?"), but different bins or types
 * can be active together.
 */
struct kasen_alarm {
	enum kasen_alarm_type type;
	kasen_bin_t bin;   /**< KASEN_BIN_NONE for KASEN_ALARM_LOW_KITS. */
	bool active;       /**< true = raised, false = cleared. */
	/**
	 * What was measured and what it was compared with:
	 *  - LOW_STOCK:        value = count,          limit = low_qty
	 *  - RECIPE_MISMATCH:  value = parts used,     limit = parts expected
	 *  - LOW_KITS:         value = kits remaining, limit = low_kits
	 *  - SENSOR_FAULT:     value = enum kasen_err, limit = 0
	 */
	int32_t value;
	int32_t limit;
	int64_t raised_ms; /**< Uptime when raised. */
};

/** Most alarms that can be active at once: every type on every bin. */
#define KASEN_MAX_ALARMS (KASEN_ALARM_TYPE_COUNT * KASEN_NUM_BINS)

/* ------------------------------------------------------------------------ */
/* Assembly-cycle result (Map 7)                                             */
/* ------------------------------------------------------------------------ */

/**
 * Outcome of one Start Assembly ... Assembly Complete cycle. Sent to the hub
 * for both pass and fail so the hub has a record of every assembly.
 */
struct kasen_cycle_result {
	bool pass; /**< Map 7 "Does each bin match the recipe?" */
	/** Bins where used differs from expected by more than the tolerance. */
	kasen_bin_mask_t mismatch_mask;
	/**
	 * Bins that could not be checked (not steady in time, or not set up).
	 * A cycle with any unverified recipe bin does not pass.
	 */
	kasen_bin_mask_t unverified_mask;
	int32_t expected[KASEN_NUM_BINS]; /**< From the recipe. */
	/** Count at Start minus count at Complete. Negative = parts added. */
	int32_t used[KASEN_NUM_BINS];
	uint32_t cycle_no;    /**< Counts up from 1 since boot. */
	uint32_t duration_ms; /**< Start press to Complete press. */
};

#ifdef __cplusplus
}
#endif

#endif /* KASEN_TYPES_H_ */
