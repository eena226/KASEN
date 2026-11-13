/**
 * @file comms.h
 * @brief Wi-Fi link, MQTT client, and the message contract with the hub.
 *
 * Process maps covered:
 *  - Map 1  "Start the RTOS; connect to WiFi & MQTT broker",
 *           "WiFi & broker link up?", "Keep trying to connect; keep weighing"
 *  - Map 5  Receive routine: "MQTT message arrives", "For this station?"
 *  - Map 6  Send Count routine: "WiFi link up?", buffer, send
 *  - Maps 2, 3, 5  "Send MQTT reply 'Done' or 'Failed'"
 *  - Maps 7, 8  result and alarm messages
 *
 * How the station reaches Mosquitto
 * ---------------------------------
 *   nRF54LM20 --SPI--> nRF7002 EB II --Wi-Fi--> Raspberry Pi access point
 *   Zephyr TCP/IP socket --> port 1883 --> Mosquitto on the Pi
 *
 * Nordic's sample nrf/samples/net/mqtt builds for exactly this board pair
 * (nrf54lm20dk + shield nrf7002eb2) and is the model for comms.c:
 *  - Wi-Fi association through Zephyr's connection manager with stored
 *    credentials.
 *  - MQTT 3.1.1 through Nordic's mqtt_helper library (CONFIG_MQTT_HELPER),
 *    which wraps Zephyr's MQTT client and runs its own receive thread.
 * The repo's wifi/ folder (Nordic's STA sample) already proves the Wi-Fi
 * half on this hardware.
 *
 * ---------------------------------------------------------------------------
 * MQTT CONTRACT (for whoever writes the Pi side)
 * ---------------------------------------------------------------------------
 * <sid> = station ID, e.g. "station1". <n> = bin number 1..5.
 * All payloads are JSON objects, UTF-8, no trailing NUL.
 *
 * Station -> hub
 *
 *   kasen/<sid>/status            retained, QoS 1
 *     {"online":true,"fw":"0.1.0","bins":5}
 *     Last-will (broker publishes it if the station vanishes):
 *     {"online":false}
 *
 *   kasen/<sid>/bin/<n>/count     QoS 0          Map 6
 *     {"station":"station1","bin":3,"part":"RES-10K","count":118,
 *      "w_mg":15340,"valid":true,"stable":true,"low":false,
 *      "seq":1042,"age_ms":0}
 *     Sent when the bin's count changes and at least every 2 s.
 *     age_ms = how long ago the value was captured; non-zero only for
 *     messages that waited in the offline buffer. Hub time of capture =
 *     time received - age_ms. (The station has no clock of its own.)
 *
 *   kasen/<sid>/event/cycle       QoS 1          Map 7
 *     {"station":"station1","cycle":57,"result":"pass","duration_ms":8420,
 *      "bins":[{"bin":1,"expected":2,"used":2,"ok":true}, ...5 entries]}
 *     result is "pass" or "fail". An entry may carry "ok":null when the bin
 *     could not be verified.
 *
 *   kasen/<sid>/event/alarm       QoS 1          Map 8
 *     {"station":"station1","type":"low_stock","bin":3,"active":true,
 *      "value":4,"limit":5}
 *     type: "low_stock" | "recipe_mismatch" | "low_kits" | "sensor_fault".
 *     bin is 0 for low_kits. active:false is sent when the alarm clears.
 *
 *   kasen/<sid>/reply             QoS 1          Maps 2, 3, 5
 *     {"req":17,"cmd":"tare","bin":3,"status":"done"}
 *     {"req":18,"cmd":"calibrate","bin":3,"status":"in_progress"}
 *     {"req":18,"cmd":"calibrate","bin":3,"status":"failed","err":"timeout"}
 *     status: "done" | "failed" | "in_progress". err values are the texts
 *     of kasen_err_str().
 *
 * Hub -> station (the station subscribes to kasen/<sid>/cmd/+, QoS 1)
 *
 *   kasen/<sid>/cmd/tare          {"req":17,"bin":3}
 *   kasen/<sid>/cmd/calibrate     {"req":18,"bin":3,"known_mg":100000}
 *   kasen/<sid>/cmd/part          {"req":19,"bin":3,"part":"RES-10K",
 *                                  "low_qty":20,"unit_ug":0}
 *                                 unit_ug: weight of one part in micrograms
 *                                 if known, else 0 or absent.
 *   kasen/<sid>/cmd/sample        {"req":20,"bin":3,"qty":25}
 *                                 "There are 25 parts in bin 3 right now."
 *   kasen/<sid>/cmd/recipe        {"req":21,"name":"PCB-A","tol":0,
 *                                  "low_kits":5,
 *                                  "items":[{"bin":1,"qty":2},
 *                                           {"bin":4,"qty":1}]}
 *                                 Bins not listed have qty 0. An empty
 *                                 items list unloads the recipe.
 *   kasen/<sid>/cmd/alarm_clear   {"req":22,"type":"low_stock","bin":3}
 *
 * "req" is any number the hub picks; the station echoes it. Because each
 * station has its own topic branch, Map 5's "For this station?" is answered
 * by the broker; the station still rejects a topic whose <sid> is not its
 * own.
 *
 * Timing the hub must allow for: tare answers within about 4 s. Calibrate
 * answers "in_progress" at once and "done"/"failed" up to
 * KASEN_CAL_WAIT_TIMEOUT_MS later, which is longer than the 5 s on Map 9.
 * ---------------------------------------------------------------------------
 *
 * Threading: every function here may be called from any thread. Publish
 * functions never block on the network; they queue and return.
 */

#ifndef KASEN_COMMS_H_
#define KASEN_COMMS_H_

#include "kasen_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Topic suffixes (appended to "kasen/<sid>/")                               */
/* ------------------------------------------------------------------------ */

#define COMMS_TOPIC_STATUS          "status"
#define COMMS_TOPIC_COUNT_FMT       "bin/%u/count"   /* %u = 1..5 */
#define COMMS_TOPIC_EVENT_CYCLE     "event/cycle"
#define COMMS_TOPIC_EVENT_ALARM     "event/alarm"
#define COMMS_TOPIC_REPLY           "reply"
#define COMMS_TOPIC_CMD_SUBSCRIBE   "cmd/+"

#define COMMS_CMD_TARE              "tare"
#define COMMS_CMD_CALIBRATE         "calibrate"
#define COMMS_CMD_PART              "part"
#define COMMS_CMD_SAMPLE            "sample"
#define COMMS_CMD_RECIPE            "recipe"
#define COMMS_CMD_ALARM_CLEAR       "alarm_clear"

/** Longest topic the station builds or accepts, including the NUL. */
#define COMMS_TOPIC_MAX_LEN 64

/** Longest JSON payload the station builds or accepts, including the NUL. */
#define COMMS_PAYLOAD_MAX_LEN 512

/* ------------------------------------------------------------------------ */
/* Link state                                                                */
/* ------------------------------------------------------------------------ */

enum comms_state {
	COMMS_STATE_OFF,             /**< comms_start() not called yet. */
	COMMS_STATE_WIFI_CONNECTING, /**< Looking for the Pi's access point. */
	COMMS_STATE_MQTT_CONNECTING, /**< Wi-Fi up, connecting to the broker. */
	COMMS_STATE_ONLINE           /**< Subscribed; messages flow. */
};

/** Calls made by comms into the application. Both run in a comms thread
 *  and must return quickly (copy into the logic thread's queue). */
struct comms_callbacks {
	/**
	 * A valid command for this station arrived (Map 5 start). Messages
	 * that fail to decode never reach this callback; comms answers them
	 * with a 'failed' reply itself when a "req" number could be read.
	 */
	void (*on_command)(const struct kasen_cmd *cmd);
	/**
	 * The link went up (true) or down (false). Map 1 "WiFi & broker
	 * link up?" as an event instead of a poll.
	 */
	void (*on_link_change)(bool online);
};

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

/**
 * Prepare the network stack. Does not connect.
 *
 * Operations:
 *  1. Remember @p cb.
 *  2. Work out the station ID (KASEN_DEFAULT_STATION_ID unless overridden).
 *  3. Build the fixed topic strings once.
 *  4. Initialise mqtt_helper with callbacks for connack, disconnect and
 *     incoming publish; configure the last-will message.
 *  5. Create the transmit queue and the offline buffer.
 *
 * @param cb Callbacks; may be NULL, members may be NULL.
 * @retval 0 OK. @retval -KASEN_ERR_COMMS Network stack not available.
 */
int comms_init(const struct comms_callbacks *cb);

/**
 * Begin connecting and keep the link alive forever. Returns at once.
 * Map 1: "connect to WiFi & MQTT broker" and "Keep trying to connect; keep
 * weighing".
 *
 * Behaviour of the background work:
 *  1. Associate with the access point; on failure retry with a growing
 *     delay (1 s, 2 s, 4 s ... capped at 30 s).
 *  2. Connect to the broker; same retry rule.
 *  3. On connect: subscribe to cmd/+, publish the retained "online"
 *     status, send everything in the offline buffer oldest first, then
 *     report on_link_change(true).
 *  4. On any drop: report on_link_change(false) and go back to 1 or 2.
 *
 * @retval 0 Started (or already started).
 * @retval -KASEN_ERR_BAD_STATE comms_init() has not run.
 */
int comms_start(void);

/** Current state of the link. */
enum comms_state comms_get_state(void);

/** Maps 1 and 6 "link up?": true only in COMMS_STATE_ONLINE. */
bool comms_is_online(void);

/** This station's ID as used in topics. Never NULL. */
const char *comms_station_id(void);

/** Messages waiting in the offline buffer (Map 6 "Store the message"). */
size_t comms_buffered_count(void);

/* ------------------------------------------------------------------------ */
/* Publishing                                                                */
/* ------------------------------------------------------------------------ */

/*
 * Common behaviour of the four functions below (Map 6):
 *
 *   build the message
 *   link up?  no  -> store it in the buffer, return 0
 *             yes -> send buffered messages first, then this one
 *
 * Buffer rules, which differ from a plain FIFO for a reason:
 *  - Count messages: only the newest per bin is kept while offline. Two
 *    hundred stale heartbeats are worth less than one current value, and
 *    they would push out the events below.
 *  - Cycle results, alarms and replies: every one is kept, in order. When
 *    the buffer is full the oldest is dropped.
 *
 * All four return:
 * @retval 0                    Sent or buffered.
 * @retval -KASEN_ERR_BAD_VALUE NULL argument or a field out of range.
 * @retval -KASEN_ERR_NO_MEM    Did not fit in COMMS_PAYLOAD_MAX_LEN.
 * @retval -KASEN_ERR_BAD_STATE comms_init() has not run.
 */

/** Map 6: publish one bin's count on bin/<n>/count. */
int comms_publish_count(const struct kasen_count_msg *msg);

/** Map 7: publish a cycle result on event/cycle. */
int comms_publish_cycle(const struct kasen_cycle_result *result);

/** Map 8: publish an alarm being raised or cleared on event/alarm. */
int comms_publish_alarm(const struct kasen_alarm *alarm);

/** Maps 2, 3, 5: publish 'Done' / 'Failed' / in-progress on reply. */
int comms_publish_reply(const struct kasen_reply *reply);

/* ------------------------------------------------------------------------ */
/* Codec (pure functions: text in, struct out and the reverse)               */
/* ------------------------------------------------------------------------ */

/*
 * Kept separate from the socket code so the exact JSON can be unit-tested
 * on a PC and shared with the Pi developer as the definition of the format.
 * This is the only place where bin numbers change between 0-based and
 * 1-based and where grams change to integer milligrams / micrograms.
 */

/**
 * Decode an incoming command. Map 5: "For this station?" and "Which
 * command?". Value checks ("Values valid?") are done later by logic,
 * because they depend on station state.
 *
 * @param station_id This station's ID.
 * @param topic      Full topic, NUL-terminated.
 * @param payload    JSON bytes, not necessarily NUL-terminated.
 * @param len        Number of payload bytes.
 * @param[out] cmd   Filled in on success. On -KASEN_ERR_BAD_VALUE,
 *                   cmd->req_id and cmd->type are still set when they could
 *                   be read, so a 'failed' reply can be sent.
 * @retval 0                      Decoded.
 * @retval -KASEN_ERR_BAD_STATE    Topic is for another station: ignore
 *                                silently (Map 5 "Ignore the message").
 * @retval -KASEN_ERR_UNKNOWN_CMD  Topic's last level is not a known command.
 * @retval -KASEN_ERR_BAD_VALUE    Not JSON, a required field is missing, or
 *                                a field has the wrong type.
 * @retval -KASEN_ERR_BAD_BIN      "bin" outside 1..5.
 */
int comms_decode_command(const char *station_id, const char *topic,
			 const uint8_t *payload, size_t len,
			 struct kasen_cmd *cmd);

/*
 * Encoders. Each writes a NUL-terminated JSON object into @p buf and
 * returns the number of characters written (excluding the NUL), or
 * -KASEN_ERR_NO_MEM if it does not fit, or -KASEN_ERR_BAD_VALUE for a NULL
 * argument.
 */

/**
 * @param seq    Message counter for this bin, increasing by one each time.
 * @param age_ms Now minus msg->captured_ms; 0 for a live message.
 */
int comms_encode_count(const char *station_id,
		       const struct kasen_count_msg *msg, uint32_t seq,
		       uint32_t age_ms, char *buf, size_t buf_len);

int comms_encode_cycle(const char *station_id,
		       const struct kasen_cycle_result *result, char *buf,
		       size_t buf_len);

int comms_encode_alarm(const char *station_id, const struct kasen_alarm *alarm,
		       char *buf, size_t buf_len);

int comms_encode_reply(const struct kasen_reply *reply, char *buf,
		       size_t buf_len);

/**
 * Build "kasen/<station_id>/<suffix>".
 * @return Characters written, or -KASEN_ERR_NO_MEM if it does not fit in
 *         @p buf_len, or -KASEN_ERR_BAD_VALUE for a NULL argument.
 */
int comms_build_topic(const char *station_id, const char *suffix, char *buf,
		      size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* KASEN_COMMS_H_ */
