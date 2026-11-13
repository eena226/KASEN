/**
 * @file logic.h
 * @brief The station's decisions: counts, recipe check, alarms, commands.
 *
 * Process maps covered:
 *  - Map 1  the loop's four decisions
 *  - Map 4  Count routine (weight -> parts)
 *  - Map 5  Receive routine ("Values valid?", "Run the matching routine")
 *  - Map 7  Recipe Check routine
 *  - Map 8  Alarm routine (station lane)
 *
 * This module owns no hardware. It reads weights from sensing.h, and it
 * speaks through comms.h (MQTT) and ui.h (LCD). It is the same source file
 * in the station build and the demo build.
 *
 * How it is driven
 * ----------------
 * One thread, the logic thread, runs Map 1. Other threads never call into
 * logic; they drop events into a queue that the logic thread empties. In
 * outline (this loop lives in each build's main.c):
 *
 *   logic_init();
 *   for (;;) {
 *       wait for a queue item or 100 ms, whichever comes first
 *       item is a command        -> logic_on_command(&cmd)        Map 5
 *       item is a button         -> logic_on_button(evt, now)     Map 7
 *       item is a sensing event  -> logic_on_sensing_event(&evt)  Maps 2, 3
 *       item is a link change    -> logic_on_link_change(online)
 *       sensing_get_all(readings);
 *       logic_tick(readings, now);                                Maps 1, 4, 6, 8
 *   }
 *
 * Rule: every logic_ function must be called from the logic thread only.
 * That one rule removes the need for locks inside this module.
 *
 * Two layers
 * ----------
 * The "handlers" (section 1) are what main.c calls. They are built from the
 * "routines" (sections 2 to 5), one group per process map. Routines marked
 * PURE take all inputs as arguments and touch no module state, so each can
 * be unit-tested on a PC with a table of inputs and expected outputs.
 *
 * Storage
 * -------
 * Part assignments, unit weights, low-stock levels and the recipe are saved
 * to non-volatile storage whenever they change and loaded by logic_init(),
 * so a power cycle does not require the hub to set the station up again.
 */

#ifndef KASEN_LOGIC_H_
#define KASEN_LOGIC_H_

#include "kasen_types.h"
#include "sensing.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* 1. Handlers: the entry points main.c calls                               */
/* ======================================================================== */

/**
 * Reset all state and load saved configuration.
 *
 * Operations:
 *  1. Clear counts, alarms and the cycle state machine.
 *  2. Load each bin's part name, unit weight and low-stock level from
 *     storage; a bin with nothing stored is "unassigned" with
 *     KASEN_DEFAULT_LOW_QTY.
 *  3. Load the recipe from storage, if any.
 *  4. Push a first ui_model so the LCD is not blank.
 *
 * Call after sensing_init(), comms_init() and ui_init().
 *
 * @retval 0 OK.
 * @retval -KASEN_ERR_STORAGE Stored data unreadable; defaults used and the
 *                            station still runs.
 */
int logic_init(void);

/**
 * One pass of the Map 1 loop, after "Read all 5 load cells".
 *
 * Operations, in map order:
 *  1. logic_count_update(): turn each bin's weight into a count (Map 4).
 *  2. "Any count changed, or have 2 sec passed?"
 *     logic_count_publish_due() -> for each bin in the mask build a
 *     kasen_count_msg and comms_publish_count() it (Map 6), then
 *     logic_count_mark_published().
 *  3. logic_cycle_poll(): if a Start or Complete press is waiting for
 *     steady readings, finish it now if possible (Map 7). If it yields a
 *     result: comms_publish_cycle(), and for each mismatched bin
 *     logic_alarm_raise(RECIPE_MISMATCH).
 *  4. "Any bin below its low level?" logic_stock_check() (-> Map 8).
 *  5. Refresh the ui_model and ui_update() it if anything visible changed.
 *
 * "New message from the hub?" and "Is the button pressed?" are not polled
 * here; they arrive through logic_on_command() and logic_on_button().
 *
 * @param readings Latest readings of all bins from sensing_get_all().
 * @param now_ms   Current uptime.
 */
void logic_tick(const struct sensing_reading readings[KASEN_NUM_BINS],
		int64_t now_ms);

/**
 * Receive routine, Map 5, from "Which command?" to the reply.
 *
 * Operations:
 *  1. logic_cmd_validate() ("Values valid?").
 *     Not valid -> comms_publish_reply(FAILED, reason) and return.
 *  2. "Run the matching routine":
 *       TARE           -> sensing_tare_start()               Map 2
 *       CALIBRATE      -> sensing_calibrate_start(), and reply IN_PROGRESS
 *                                                            Map 3
 *       SET_PART       -> logic_bin_set_part()
 *       SET_SAMPLE_QTY -> logic_count_learn_unit_weight()    Map 4
 *       LOAD_RECIPE    -> logic_recipe_load()                Map 7
 *       CLEAR_ALARM    -> logic_alarm_clear()                Map 8
 *  3. For the four commands that finish immediately, reply DONE or FAILED
 *     now. For TARE and CALIBRATE the final reply is sent later by
 *     logic_on_sensing_event(); if the start call itself fails (busy, bad
 *     bin) reply FAILED now.
 *
 * @param cmd Decoded command from comms. Ignored if NULL.
 */
void logic_on_command(const struct kasen_cmd *cmd);

/**
 * A button was pressed. Map 1: "Is the button pressed?" -> Map 7.
 *
 *   UI_BTN_START_ASSEMBLY    -> logic_cycle_request_start()
 *   UI_BTN_ASSEMBLY_COMPLETE -> logic_cycle_request_complete()
 *   UI_BTN_SIM_x (demo only) -> select bin / sensing_sim_add_parts(+1/-1)
 *                               / sensing_sim_toggle_ref_weight()
 *
 * A press that makes no sense right now (Complete with no cycle running,
 * Start with no recipe) is not an error for the caller: this function
 * shows the reason on the LCD with ui_show_message() and returns.
 *
 * @param evt    What the user asked for.
 * @param now_ms Uptime of the press.
 */
void logic_on_button(enum ui_button_event evt, int64_t now_ms);

/**
 * A tare or calibration job reported progress. Closes Maps 2 and 3.
 *
 *   TARE_DONE    -> ui message, reply DONE          (Map 2 "Update the
 *                   LCD", "Send MQTT reply 'Done'")
 *   TARE_FAILED  -> ui message, reply FAILED + reason
 *   CAL_WAITING  -> ui_show_message("Place <known> g in bin <n>")
 *   CAL_DONE     -> ui message, reply DONE; the bin's unit weight stays
 *                   valid because it is in grams
 *   CAL_FAILED   -> ui message, reply FAILED + reason
 *   FAULT        -> logic_alarm_raise(SENSOR_FAULT, bin)
 *   FAULT_CLEARED-> logic_alarm_clear(SENSOR_FAULT, bin)
 *
 * @param evt Event from the sensing callback. Ignored if NULL.
 */
void logic_on_sensing_event(const struct sensing_event *evt);

/**
 * The MQTT link went up or down.
 *
 * Up: force a publish of every bin on the next tick so the hub's display
 * is correct at once instead of after the next change.
 * Down or up: update the link fields of the ui_model.
 *
 * @param online New state from the comms callback.
 */
void logic_on_link_change(bool online);

/* ======================================================================== */
/* 2. Bin setup (Map 5 "Set bin part information", Map 9 "Enter part data") */
/* ======================================================================== */

/** What the station knows about one bin. */
struct logic_bin {
	/* Configuration (saved to storage) */
	char part_name[KASEN_PART_NAME_LEN]; /**< "" = unassigned. */
	float unit_weight_g;  /**< Weight of one part; 0 = not known yet. */
	uint16_t low_qty;     /**< Low-stock level in parts; 0 = no alarm. */

	/* Live state (recomputed every tick) */
	int32_t count;        /**< Parts in the bin. */
	float net_g;          /**< Weight above tare. */
	bool valid;           /**< count can be trusted. */
	bool stable;          /**< Reading was steady. */
	bool low;             /**< valid and count <= low_qty (low_qty > 0). */
};

/**
 * Assign a part to a bin.
 *
 * Operations:
 *  1. Store the name and low-stock level.
 *  2. If @p unit_weight_g > 0 store it; counting can start at once.
 *     If it is 0 and the part name changed, forget the old unit weight:
 *     the bin shows "--" until a sample quantity is set (Map 4).
 *     If it is 0 and the name is unchanged, keep the existing unit weight
 *     (this call only changed the low-stock level).
 *  3. Clear any LOW_STOCK alarm on the bin so it is re-evaluated against
 *     the new level.
 *  4. Save to storage.
 *
 * @param bin           Bin index.
 * @param part_name     NUL-terminated, 1..KASEN_PART_NAME_LEN-1 characters.
 *                      "" unassigns the bin.
 * @param low_qty       Low-stock level; 0 disables the alarm.
 * @param unit_weight_g Known weight of one part, or 0.
 * @retval 0                   OK.
 * @retval -KASEN_ERR_BAD_BIN   @p bin out of range.
 * @retval -KASEN_ERR_BAD_VALUE Name NULL or too long, or negative weight.
 * @retval -KASEN_ERR_BAD_STATE An assembly cycle is running.
 * @retval -KASEN_ERR_STORAGE   Applied in RAM but not saved.
 */
int logic_bin_set_part(kasen_bin_t bin, const char *part_name,
		       uint16_t low_qty, float unit_weight_g);

/**
 * Copy a bin's configuration and live state.
 * @retval 0 OK. @retval -KASEN_ERR_BAD_BIN / -KASEN_ERR_BAD_VALUE (NULL).
 */
int logic_bin_get(kasen_bin_t bin, struct logic_bin *out);

/* ======================================================================== */
/* 3. Count routine (Map 4) and the Map 1 publish decision                  */
/* ======================================================================== */

/**
 * Map 4 from "Sample quantity received" to "save value": learn the weight
 * of one part from a known number of parts in the bin.
 *
 * Operations:
 *  1. "Sample quantity > 0?" no -> -KASEN_ERR_BAD_VALUE.
 *  2. "Read the net weight of the bin": use @p reading. It must be usable
 *     (tared, calibrated, no fault) and steady.
 *  3. Reject a sample lighter than KASEN_SAMPLE_MIN_NET_G.
 *  4. "Item weight = net weight / sample quantity; save value" (storage).
 *  5. The bin's count becomes @p sample_qty on the next tick ("From now
 *     on: count = reading / item weight, rounded"); the tick also sends it
 *     to the hub and the LCD as the map shows.
 *
 * Accuracy note for the user guide: the error in the unit weight is
 * multiplied by the number of parts later in the bin. A sample of 10 parts
 * or more is needed to hold +/-1 part on a full bin.
 *
 * @param bin        Bin index.
 * @param sample_qty Parts in the bin right now.
 * @param reading    The bin's current reading.
 * @retval 0                         Unit weight stored.
 * @retval -KASEN_ERR_BAD_BIN         @p bin out of range.
 * @retval -KASEN_ERR_BAD_VALUE       @p sample_qty is 0 or @p reading NULL.
 * @retval -KASEN_ERR_NOT_TARED       Bin has no zero offset.
 * @retval -KASEN_ERR_NOT_CALIBRATED  Bin has no calibration.
 * @retval -KASEN_ERR_SENSOR          Reading is in fault or overload.
 * @retval -KASEN_ERR_UNSTABLE        Reading is not steady; try again.
 * @retval -KASEN_ERR_TOO_LIGHT       Net weight below the minimum.
 * @retval -KASEN_ERR_BAD_STATE       An assembly cycle is running.
 * @retval -KASEN_ERR_STORAGE         Applied in RAM but not saved.
 */
int logic_count_learn_unit_weight(kasen_bin_t bin, uint32_t sample_qty,
				  const struct sensing_reading *reading);

/**
 * PURE. Map 4: "count = reading / item weight, rounded", with hysteresis.
 *
 * Plain rounding flips between n and n+1 when the weight sits at n + 0.5
 * parts. This keeps @p prev_count until the weight is more than
 * 0.5 + KASEN_COUNT_HYSTERESIS parts away from it, then rounds normally.
 *
 * @param net_g         Weight above tare, grams.
 * @param unit_weight_g Weight of one part, grams.
 * @param prev_count    The count returned last time, or -1 for "none"
 *                      (plain rounding).
 * @return Parts, never negative. 0 if @p unit_weight_g <= 0.
 */
int32_t logic_count_from_weight(float net_g, float unit_weight_g,
				int32_t prev_count);

/**
 * Recompute every bin's count from new readings. Step 1 of logic_tick().
 *
 * For each bin:
 *  - Not usable, or no unit weight: valid = false; count keeps its last
 *    value for reference.
 *  - Usable but not steady: keep the previous count (a hand in the bin is
 *    not a change in stock); stable = false.
 *  - Usable and steady: count = logic_count_from_weight(); valid = true.
 *  - Update low.
 *
 * @param readings All bins, same acquisition pass.
 * @param now_ms   Uptime.
 */
void logic_count_update(const struct sensing_reading readings[KASEN_NUM_BINS],
			int64_t now_ms);

/**
 * Map 1: "Any count changed, or have 2 sec passed?"
 *
 * @param now_ms Uptime.
 * @return Mask of bins to publish now: a bin is included if its count,
 *         valid or low flag differs from what was last published, and all
 *         bins are included if KASEN_HEARTBEAT_MS has passed since the
 *         last full publish or a publish was forced by a link-up.
 *         0 = nothing to send.
 */
kasen_bin_mask_t logic_count_publish_due(int64_t now_ms);

/**
 * Record that the bins in @p mask were just handed to comms, so
 * logic_count_publish_due() stops reporting them.
 */
void logic_count_mark_published(kasen_bin_mask_t mask, int64_t now_ms);

/* ======================================================================== */
/* 4. Recipe Check routine (Map 7)                                          */
/* ======================================================================== */

/**
 * Where the station is in an assembly cycle.
 *
 *   IDLE --Start pressed--> ARMING --steady--> ACTIVE
 *   ACTIVE --Complete pressed--> CLOSING --steady or timeout--> IDLE
 *
 * ARMING and CLOSING are the short waits (at most
 * KASEN_CYCLE_SETTLE_TIMEOUT_MS) for steady readings before a snapshot.
 */
enum logic_cycle_state {
	LOGIC_CYCLE_IDLE,    /**< Waiting for Start Assembly. */
	LOGIC_CYCLE_ARMING,  /**< Start pressed; waiting to take start counts. */
	LOGIC_CYCLE_ACTIVE,  /**< Map 7 "Wait for Assembly Complete". */
	LOGIC_CYCLE_CLOSING  /**< Complete pressed; waiting to take end counts. */
};

/**
 * Map 5 "Load recipe (Map 7)": store what one assembly should consume.
 *
 * Operations:
 *  1. Copy and save the recipe. A recipe whose quantities are all 0 unloads
 *     the current one.
 *  2. Clear RECIPE_MISMATCH and LOW_KITS alarms; they referred to the old
 *     recipe.
 *
 * @param recipe New recipe.
 * @retval 0                   Loaded (or unloaded).
 * @retval -KASEN_ERR_BAD_VALUE NULL, or name not NUL-terminated.
 * @retval -KASEN_ERR_BAD_STATE A cycle is running; finish it first.
 * @retval -KASEN_ERR_STORAGE   Applied in RAM but not saved.
 */
int logic_recipe_load(const struct kasen_recipe *recipe);

/**
 * Copy the loaded recipe.
 * @retval 0 OK. @retval -KASEN_ERR_NO_RECIPE None loaded.
 * @retval -KASEN_ERR_BAD_VALUE @p out is NULL.
 */
int logic_recipe_get(struct kasen_recipe *out);

/**
 * Map 7 "Start Assembly pressed".
 *
 * Moves IDLE -> ARMING. The start counts ("Save the count of every bin")
 * are taken by logic_cycle_poll() once the bins are steady. If they are
 * not steady within the settle timeout the start counts are taken anyway
 * from the last steady values and the cycle still begins.
 *
 * @param now_ms Uptime of the press.
 * @retval 0                     Accepted.
 * @retval -KASEN_ERR_NO_RECIPE   No recipe loaded.
 * @retval -KASEN_ERR_BAD_STATE   A cycle is already running.
 * @retval -KASEN_ERR_NO_UNIT_WEIGHT A bin the recipe uses has no valid
 *                               count (not tared, not calibrated, or no
 *                               unit weight), so it could not be checked.
 */
int logic_cycle_request_start(int64_t now_ms);

/**
 * Map 7 "User presses 'Assembly Complete'".
 *
 * Moves ACTIVE -> CLOSING. The comparison happens in logic_cycle_poll().
 *
 * @param now_ms Uptime of the press.
 * @retval 0                   Accepted.
 * @retval -KASEN_ERR_BAD_STATE No cycle is active.
 */
int logic_cycle_request_complete(int64_t now_ms);

/**
 * Advance the cycle state machine. Step 3 of logic_tick().
 *
 *  ARMING:  all recipe bins steady, or timeout -> save start counts,
 *           go ACTIVE.
 *  CLOSING: all recipe bins steady, or timeout -> take end counts,
 *           logic_cycle_evaluate(), go IDLE, return true.
 *           A bin still not steady at the timeout is marked unverified.
 *  IDLE, ACTIVE: nothing.
 *
 * @param now_ms Uptime.
 * @param[out] result Filled in when the function returns true.
 * @return true exactly once per completed cycle, when @p result is ready.
 */
bool logic_cycle_poll(int64_t now_ms, struct kasen_cycle_result *result);

/**
 * Abandon the current cycle with no result and no alarm. For a wrong Start
 * press, or when setup commands must be applied. No effect when IDLE.
 */
void logic_cycle_abort(void);

/** Current state of the cycle state machine. */
enum logic_cycle_state logic_cycle_get_state(void);

/**
 * PURE. Map 7: "How many parts were used?" and "Does each bin match the
 * recipe?".
 *
 * For each bin: used = start - end; expected = recipe->qty[bin];
 * mismatch if |used - expected| > recipe->tolerance. Bins in
 * @p unverified_mask are not compared. pass = no mismatch and no unverified
 * bin that the recipe uses (qty > 0).
 *
 * Fills pass, mismatch_mask, unverified_mask, expected[] and used[] of
 * @p result. cycle_no and duration_ms are left for the caller.
 *
 * @param start_count     Counts saved at Start.
 * @param end_count       Counts taken at Complete.
 * @param recipe          Recipe in force.
 * @param unverified_mask Bins whose counts could not be trusted.
 * @param[out] result     Receives the outcome. Nothing happens if any
 *                        pointer is NULL.
 */
void logic_cycle_evaluate(const int32_t start_count[KASEN_NUM_BINS],
			  const int32_t end_count[KASEN_NUM_BINS],
			  const struct kasen_recipe *recipe,
			  kasen_bin_mask_t unverified_mask,
			  struct kasen_cycle_result *result);

/**
 * PURE. Scope Deliverable #5: how many complete assemblies the current
 * stock supports.
 *
 * @param count  Current count of each bin.
 * @param valid_mask Bins whose count can be trusted.
 * @param recipe Recipe in force.
 * @return The smallest count[bin] / qty[bin] over bins with qty > 0.
 *         -1 if @p recipe is NULL, uses no bins, or a bin it uses is not
 *         in @p valid_mask.
 */
int32_t logic_kits_remaining(const int32_t count[KASEN_NUM_BINS],
			     kasen_bin_mask_t valid_mask,
			     const struct kasen_recipe *recipe);

/* ======================================================================== */
/* 5. Alarm routine (Map 8, station lane)                                   */
/* ======================================================================== */

/**
 * Map 8 from "Bin low or Recipe Check failed" to "Send the alarm to the
 * hub".
 *
 * Operations:
 *  1. "Alarm already active?" for this (type, bin) -> yes: return 0, do
 *     nothing (the map's early End).
 *  2. Record the alarm.
 *  3. "Show the alarm on the LCD": handled by the next ui_update() through
 *     alarm_count and the bin's low / mismatch flag.
 *  4. "Send the alarm to the hub (MQTT)": comms_publish_alarm(active).
 *
 * @param type   Kind of alarm.
 * @param bin    Bin index, or KASEN_BIN_NONE for LOW_KITS.
 * @param value  Measured value (see struct kasen_alarm).
 * @param limit  Value it was compared with.
 * @param now_ms Uptime.
 * @retval 1                   Newly raised.
 * @retval 0                   Was already active; nothing done.
 * @retval -KASEN_ERR_BAD_BIN   @p bin invalid for this type.
 * @retval -KASEN_ERR_BAD_VALUE @p type out of range.
 */
int logic_alarm_raise(enum kasen_alarm_type type, kasen_bin_t bin,
		      int32_t value, int32_t limit, int64_t now_ms);

/**
 * Map 8 "Remove the alarm from the LCD and the hub". Called when the user
 * clears the popup on the hub (CLEAR_ALARM command) or when the station
 * sees the cause go away.
 *
 * Operations:
 *  1. If not active: return 0.
 *  2. Remove it; clear the bin's mismatch flag if it was a
 *     RECIPE_MISMATCH.
 *  3. comms_publish_alarm(active = false).
 *  4. For LOW_STOCK and LOW_KITS, remember "acknowledged": the alarm is
 *     not raised again until the stock has first risen back above the
 *     level. Without this, clearing a low-stock popup would bring it
 *     straight back on the next tick.
 *
 * @retval 1 Cleared. @retval 0 Was not active.
 * @retval -KASEN_ERR_BAD_BIN / -KASEN_ERR_BAD_VALUE as for raise.
 */
int logic_alarm_clear(enum kasen_alarm_type type, kasen_bin_t bin);

/** True if the (type, bin) alarm is active. False for invalid arguments. */
bool logic_alarm_is_active(enum kasen_alarm_type type, kasen_bin_t bin);

/**
 * Copy the active alarms, oldest first.
 * @param[out] out Array to fill; may be NULL to only count.
 * @param max      Capacity of @p out.
 * @return Number of active alarms (may exceed @p max).
 */
size_t logic_alarm_list(struct kasen_alarm *out, size_t max);

/**
 * Map 1: "Any bin below its low level?" -> "Call Alarm Routine (Map 8)".
 * Step 4 of logic_tick().
 *
 * For each bin with a valid, steady count and low_qty > 0:
 *  - count <= low_qty and not acknowledged -> logic_alarm_raise(LOW_STOCK).
 *  - count >  low_qty -> forget the acknowledgement; if a LOW_STOCK alarm
 *    is active, clear it (the bin was restocked).
 * Then, if a recipe with low_kits > 0 is loaded, the same rule for
 * logic_kits_remaining() against low_kits (LOW_KITS, KASEN_BIN_NONE).
 *
 * @param now_ms Uptime.
 */
void logic_stock_check(int64_t now_ms);

/* ======================================================================== */
/* 6. Command validation (Map 5 "Values Valid?")                            */
/* ======================================================================== */

/**
 * PURE. Check a decoded command's values before running it. Only checks
 * that need no station state; state-dependent refusals (busy, cycle
 * running, not tared) come from the routine itself.
 *
 *   all with a bin  bin in range                       else BAD_BIN
 *   CALIBRATE       KASEN_CAL_MIN_KNOWN_G <= known_g
 *                   <= KASEN_RANGE_MAX_G               else BAD_VALUE
 *   SET_PART        name NUL-terminated within the
 *                   buffer; unit_weight_g >= 0         else BAD_VALUE
 *   SET_SAMPLE_QTY  qty > 0                            else BAD_VALUE
 *   LOAD_RECIPE     name NUL-terminated                else BAD_VALUE
 *   CLEAR_ALARM     alarm_type in range; bin in range
 *                   or KASEN_BIN_NONE for LOW_KITS     else BAD_VALUE
 *   any other type                                     UNKNOWN_CMD
 *
 * @param cmd Command to check.
 * @return KASEN_OK or the reason (positive enum, ready for a reply).
 *         KASEN_ERR_BAD_VALUE if @p cmd is NULL.
 */
enum kasen_err logic_cmd_validate(const struct kasen_cmd *cmd);

#ifdef __cplusplus
}
#endif

#endif /* KASEN_LOGIC_H_ */
