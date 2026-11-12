/*
 * counts.c: Map 4 (weight -> part count) and the Map 1 decision
 * "count changed or 2 s passed?".
 * Also holds settings_load() for now (defaults only, no flash storage yet).
 */

#include <zephyr/kernel.h>
#include "counts.h"

/*
 * Default scale: ADS1261, AVDD reference (5 V), PGA gain 1, 24-bit.
 * 1 kg cell at 1 mV/V with 5 V excitation = 5 mV, INA333 gain ~250 -> ~1.25 V.
 * 1.25 V / 5 V * 2^23 = ~2,097,152 raw counts at 1000 g.
 * Real values come from the web-UI calibration routine later.
 */
#define DEFAULT_G_PER_COUNT   (1000.0f / 2097152.0f)
#define DEFAULT_ITEM_WEIGHT_G 1.0f   /* grams per part: set to your demo part */
#define DEFAULT_LOW_LEVEL     5      /* parts */
#define SEND_PERIOD_MS        2000

struct bin_settings {
	int32_t tare_raw;
	float   g_per_count;
	float   item_weight_g;
	int     low_level;
};

static struct bin_settings bin[NUM_BINS];
static int count[NUM_BINS];
static int last_sent[NUM_BINS];
static int64_t last_send_ms;
static bool tared;

void settings_load(void)
{
	for (int i = 0; i < NUM_BINS; i++) {
		bin[i].tare_raw      = 0;
		bin[i].g_per_count   = DEFAULT_G_PER_COUNT;
		bin[i].item_weight_g = DEFAULT_ITEM_WEIGHT_G;
		bin[i].low_level     = DEFAULT_LOW_LEVEL;
		last_sent[i]         = -1;
	}
	printk("settings: defaults loaded\n");
}

void counts_update(const int raw[NUM_BINS])
{
	/* DEMO SHORTCUT: auto-tare on the first reading (bins must be empty).
	 * The real tare comes from the hub's Tare command (Map 5). */
	if (!tared) {
		for (int i = 0; i < NUM_BINS; i++) {
			bin[i].tare_raw = raw[i];
		}
		tared = true;
		printk("tare done\n");
	}

	for (int i = 0; i < NUM_BINS; i++) {
		float grams = (float)(raw[i] - bin[i].tare_raw) * bin[i].g_per_count;
		float parts = grams / bin[i].item_weight_g;

		count[i] = (parts < 0.0f) ? 0 : (int)(parts + 0.5f);
	}
}

bool counts_changed_or_2s_passed(void)
{
	bool changed = false;
	int64_t now = k_uptime_get();

	for (int i = 0; i < NUM_BINS; i++) {
		if (count[i] != last_sent[i]) {
			changed = true;
		}
	}

	if (changed || (now - last_send_ms) >= SEND_PERIOD_MS) {
		for (int i = 0; i < NUM_BINS; i++) {
			last_sent[i] = count[i];
		}
		last_send_ms = now;
		return true;
	}
	return false;
}

int counts_get(int bin_idx)
{
	return count[bin_idx];
}

int counts_low_level(int bin_idx)
{
	return bin[bin_idx].low_level;
}