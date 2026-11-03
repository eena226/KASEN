/*
 * Station firmware: main() outline (follows Map 1, Station main loop).
 * Function names are placeholders. Each one is a routine from the process maps,
 * to be written in its own file.
 */

#include <zephyr/kernel.h>


#define NUM_BINS 5

/* ---- routines from the process maps (declare in headers, define in own .c files) ---- */
int  hardware_init(void);                          /* ADC/SPI, buttons, LEDs, LCD            */
void settings_load(void);                          /* scale factors, tare, part info         */
void network_start(void);                          /* WiFi + MQTT connect, subscribe, retry  */
bool network_link_up(void);

void read_load_cells(int raw[NUM_BINS]);           /* Map 1: read all five bins              */
void counts_update(const int raw[NUM_BINS]);       /* Map 4: net weight / item weight        */
bool counts_changed_or_2s_passed(void);            /* Map 1 decision                         */
void send_counts(void);                            /* Map 6                                  */
bool hub_message_pending(void);
void receive_handle_message(void);                 /* Map 5 (tare, calibrate, ...)           */
bool button_pressed(void);
void recipe_check_run(void);                       /* Map 7                                  */
bool any_bin_below_low_level(void);
void alarm_run(void);                              /* Map 8                                  */

int main(void)
{

	if (hardware_init() != 0) {
		return 0;                  /* stop if the hardware will not start */
	}
	settings_load();
	network_start();                   /* must not block: keep weighing while it retries */

	/* ---- Main loop: runs until power off (at least 10 reads per second) ---- */
	while (1) {
		int raw[NUM_BINS];

		read_load_cells(raw);
		counts_update(raw);

		if (counts_changed_or_2s_passed()) {
			send_counts();             /* buffers the message if the link is down */
		}
		if (hub_message_pending()) {
			receive_handle_message();
		}
		if (button_pressed()) {
			recipe_check_run();
		}
		if (any_bin_below_low_level()) {
			alarm_run();
		}

		k_msleep(10);
	}

	return 0;
}