/**
 * @file ui.h
 * @brief The station's own interface: LCD, buttons, LEDs.
 *
 * Process maps covered:
 *  - Map 1  "Is the button pressed?" (input side)
 *  - Maps 2, 4, 6  "Update the LCD"
 *  - Map 3  "Prompt user to place the known weight"
 *  - Map 7  "Show a 'Pass' on the LCD", flagged bins
 *  - Map 8  "Show the alarm on the LCD" / "Remove the alarm from the LCD"
 *
 * Design: logic never draws. It fills in a struct ui_model that describes
 * what should be visible and hands it to ui_update(). The UI thread redraws
 * whatever changed. This keeps drawing code (slow, SPI) out of the logic
 * loop and makes "what is on the screen" a plain struct that tests can
 * inspect.
 *
 * Hardware (same in both builds): Adafruit 5394, 1.9 inch 320x170 ST7789
 * TFT on SPI; the four buttons and four LEDs on the nRF54LM20 DK.
 *
 * Threading: ui_update() and ui_show_message() may be called from any
 * thread. The button callback runs in the UI/input thread and must return
 * quickly.
 */

#ifndef KASEN_UI_H_
#define KASEN_UI_H_

#include "kasen_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Buttons                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * What the user asked for, independent of which physical button did it.
 * The button-to-event table is in station/README.md and demo/README.md.
 */
enum ui_button_event {
	/** Scope Deliverable #2: physical Start Assembly button (Map 7). */
	UI_BTN_START_ASSEMBLY,
	/** Scope Deliverable #2: physical Assembly Complete button (Map 7). */
	UI_BTN_ASSEMBLY_COMPLETE,

	/* Demo build only: drive the load-cell simulator. */
	UI_BTN_SIM_NEXT_BIN,  /**< Select the next simulated bin. */
	UI_BTN_SIM_PART_ADD,  /**< Put one part into the selected bin. */
	UI_BTN_SIM_PART_TAKE  /**< Take one part out of the selected bin. */
};

/**
 * Called once per debounced button action. The usual body copies the event
 * into the logic thread's queue.
 */
typedef void (*ui_button_cb_t)(enum ui_button_event evt);

/* ------------------------------------------------------------------------ */
/* Screen model                                                              */
/* ------------------------------------------------------------------------ */

/** One bin's row on the LCD. Scope Deliverable #2: part and quantity. */
struct ui_bin_view {
	char part_name[KASEN_PART_NAME_LEN]; /**< "" = no part assigned. */
	int32_t count;
	bool valid;    /**< false: show "--" instead of the count. */
	bool low;      /**< Low-stock highlight. */
	bool mismatch; /**< Map 7: flagged by the last recipe check. */
	bool selected; /**< Demo: this is the bin the sim buttons act on. */
};

/** What the status area shows about the assembly cycle (Map 7). */
enum ui_cycle_view {
	UI_CYCLE_IDLE,       /**< Waiting for Start Assembly. */
	UI_CYCLE_SETTLING,   /**< Button pressed, waiting for steady readings. */
	UI_CYCLE_ACTIVE,     /**< Between Start and Complete. */
	UI_CYCLE_PASS,       /**< Last cycle matched the recipe. */
	UI_CYCLE_FAIL        /**< Last cycle had a mismatch. */
};

/** Everything the LCD and LEDs show. */
struct ui_model {
	struct ui_bin_view bin[KASEN_NUM_BINS];
	enum ui_cycle_view cycle;
	char recipe_name[KASEN_RECIPE_NAME_LEN]; /**< "" = none loaded. */
	int32_t kits_remaining;  /**< Negative = unknown / no recipe. */
	uint8_t alarm_count;     /**< Active alarms (Map 8). 0 hides the banner. */
	bool wifi_up;            /**< Associated with the Pi's access point. */
	bool mqtt_up;            /**< Connected to the broker. */
	uint16_t buffered_msgs;  /**< Messages waiting for the link (Map 6). */
};

/* ------------------------------------------------------------------------ */
/* API                                                                       */
/* ------------------------------------------------------------------------ */

/**
 * Start the display, buttons and LEDs.
 *
 * Operations:
 *  1. Check the display device is ready; turn the backlight on.
 *  2. Draw the static layout (five bin rows, status bar).
 *  3. Configure the four DK buttons with debounce and register @p cb.
 *  4. Configure the four DK LEDs, all off.
 *
 * A missing display is not fatal: buttons and LEDs still work and the
 * function returns 0 after logging a warning, so a station with a broken
 * screen still counts and reports.
 *
 * @param cb Receives button events; may be NULL.
 * @retval 0 OK.
 * @retval -KASEN_ERR_BAD_STATE Buttons could not be configured.
 */
int ui_init(ui_button_cb_t cb);

/**
 * Give the UI a new picture of the station. "Update the LCD" on the maps.
 *
 * Copies @p model and returns; drawing happens later in the UI thread and
 * only for fields that changed. Calling this more often than the screen can
 * redraw is harmless: intermediate models are skipped.
 *
 * LED meaning (derived from the model, not set separately):
 *   LED0 link     blinking = connecting, on = mqtt_up
 *   LED1 cycle    on while UI_CYCLE_ACTIVE
 *   LED2 alarm    on while alarm_count > 0
 *   LED3 alive    toggles on each update
 *
 * @retval 0 OK. @retval -KASEN_ERR_BAD_VALUE @p model is NULL.
 */
int ui_update(const struct ui_model *model);

/**
 * Show a short message over the bin list, for prompts and results that are
 * not part of the steady picture. Examples: "Place 100 g in bin 3"
 * (Map 3), "Tare bin 2: done" (Map 2), "PASS" (Map 7).
 *
 * @param text        NUL-terminated; truncated to what fits on one line.
 * @param duration_ms How long to show it; 0 = until the next message or
 *                    ui_clear_message().
 * @retval 0 OK. @retval -KASEN_ERR_BAD_VALUE @p text is NULL.
 */
int ui_show_message(const char *text, uint32_t duration_ms);

/** Remove the message shown by ui_show_message(), if any. */
void ui_clear_message(void);

#ifdef __cplusplus
}
#endif

#endif /* KASEN_UI_H_ */
