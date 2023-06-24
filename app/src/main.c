/*
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>

#include "lorawan_config.h"
#if IS_ENABLED(CONFIG_ADC)
#include "battery.h"
#endif
#include "nvm.h"
#if IS_ENABLED(CONFIG_UBLOX_MAX7Q)
#include "gps.h"
#endif
#if IS_ENABLED(CONFIG_SHELL)
#include "shell.h"
#endif
#if IS_ENABLED(CONFIG_BT)
#include "ble.h"
#endif

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(helium_mapper);

// TODO!!! define green_led and blue_led aliases into DT
#define LED_GREEN_NODE DT_ALIAS(led0)
#define LED_BLUE_NODE DT_ALIAS(led0)
/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios);
static struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(LED_BLUE_NODE, gpios);

struct s_lorawan_config lorawan_config = {
	.dev_eui = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 },
	.app_eui = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 },
	.app_key = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F },
	.lora_mode = LORAWAN_ACT_OTAA,
	.data_rate = LORAWAN_DR_3,
	.lora_class = LORAWAN_CLASS_A,
	.confirmed_msg = LORAWAN_MSG_UNCONFIRMED,
	.app_port = 2,
	.auto_join = false,
	.send_repeat_time = 0,
	.send_min_delay = 30,
	.max_gps_on_time = 300,
	/* max join attempt in one join session */
	.join_try_count = 5,
	/* max join sessions before give up and reboot. 20 * 5 = 100 join attempts */
	.max_join_retry_sessions_count = 20,
	/* max join session interval in sec */
	.join_try_interval = 300,
	.max_inactive_time_window = 3 * 3600,
	.max_failed_msg = 120,
};

struct s_status lorawan_status = {
	.joined = false,
	.delayed_active = false,
	.gps_pwr_on = false,
	.last_pos_send = 0,
	.last_pos_send_ok = 0,
	.last_accel_event = 0,
	.msgs_sent = 0,
	.msgs_failed = 0,
	.msgs_failed_total = 0,
	.gps_total_on_time = 0,
	.acc_events = 0,
	.join_retry_sessions_count = 0,
};

struct s_mapper_data mapper_data;
char *data_ptr = (char*)&mapper_data;

#define LORA_JOIN_THREAD_STACK_SIZE 1500
#define LORA_JOIN_THREAD_PRIORITY 10
K_KERNEL_STACK_MEMBER(lora_join_thread_stack, LORA_JOIN_THREAD_STACK_SIZE);

struct s_helium_mapper_ctx {
	const struct device *lora_dev;
	const struct device *accel_dev;
	struct k_timer send_timer;
	struct k_timer delayed_timer;
	struct k_timer gps_off_timer;
	struct k_timer lora_join_timer;
	struct k_thread thread;
	struct k_sem lora_join_sem;
	bool gps_fix;
};

struct s_helium_mapper_ctx g_ctx = {
	.gps_fix = false,
};

enum lorawan_state_e {
	NOT_JOINED,
	JOINED,
};

/* Event FIFO */

K_FIFO_DEFINE(evt_fifo);

enum evt_t {
	EV_TIMER,
	EV_ACC,
	EV_GPS_FIX,
	EV_NMEA_TRIG_ENABLE,
	EV_NMEA_TRIG_DISABLE,
};

struct app_evt_t {
	sys_snode_t node;
	enum evt_t event_type;
};

#define FIFO_ELEM_MIN_SZ        sizeof(struct app_evt_t)
#define FIFO_ELEM_MAX_SZ        sizeof(struct app_evt_t)
#define FIFO_ELEM_COUNT         10
#define FIFO_ELEM_ALIGN         sizeof(unsigned int)

K_HEAP_DEFINE(event_elem_pool, FIFO_ELEM_MAX_SZ * FIFO_ELEM_COUNT + 256);

static inline void app_evt_free(struct app_evt_t *ev)
{
	k_heap_free(&event_elem_pool, ev);
}

static inline void app_evt_put(struct app_evt_t *ev)
{
	k_fifo_put(&evt_fifo, ev);
}

static inline struct app_evt_t *app_evt_get(void)
{
	return k_fifo_get(&evt_fifo, K_NO_WAIT);
}

static inline void app_evt_flush(void)
{
	struct app_evt_t *ev;

	do {
		ev = app_evt_get();
		if (ev) {
			app_evt_free(ev);
		}
	} while (ev != NULL);
}

static inline struct app_evt_t *app_evt_alloc(void)
{
	struct app_evt_t *ev;

	ev = k_heap_alloc(&event_elem_pool,
			  sizeof(struct app_evt_t),
			  K_NO_WAIT);
	if (ev == NULL) {
		LOG_ERR("APP event allocation failed!");
		app_evt_flush();

		ev = k_heap_alloc(&event_elem_pool,
				  sizeof(struct app_evt_t),
				  K_NO_WAIT);
		if (ev == NULL) {
			LOG_ERR("APP event memory corrupted.");
			__ASSERT_NO_MSG(0);
			return NULL;
		}
		return NULL;
	}

	return ev;
}

static K_SEM_DEFINE(evt_sem, 0, 1);	/* starts off "not available" */

void update_gps_off_timer(struct s_helium_mapper_ctx *ctx) {
	uint32_t timeout = lorawan_config.max_gps_on_time;

	LOG_INF("GPS off timer start for %d sec", timeout);

	k_timer_start(&ctx->gps_off_timer, K_SECONDS(timeout), K_NO_WAIT);
}

void update_send_timer(struct s_helium_mapper_ctx *ctx) {
	uint32_t time = lorawan_config.send_repeat_time;

	if (time) {
		LOG_INF("Send interval timer start for %d sec", time);
		k_timer_start(&ctx->send_timer,
				K_SECONDS(time),
				K_SECONDS(time));
	}
}

static void send_timer_handler(struct k_timer *timer)
{
	struct app_evt_t *ev;

	LOG_INF("Timer handler");

	ev = app_evt_alloc();
	ev->event_type = EV_TIMER;
	app_evt_put(ev);
	k_sem_give(&evt_sem);
}

static void delayed_timer_handler(struct k_timer *timer)
{
	struct app_evt_t *ev;

	LOG_INF("Delayed timer handler");

	lorawan_status.delayed_active = false;

	ev = app_evt_alloc();
	ev->event_type = EV_NMEA_TRIG_ENABLE;
	app_evt_put(ev);
	k_sem_give(&evt_sem);
}

static void gps_off_timer_handler(struct k_timer *timer)
{
	struct app_evt_t *ev;

	LOG_INF("GPS off timer handler");

#if IS_ENABLED(CONFIG_UBLOX_MAX7Q)
	gps_enable(GPS_DISABLE);
#endif

	ev = app_evt_alloc();
	ev->event_type = EV_NMEA_TRIG_DISABLE;
	app_evt_put(ev);
	k_sem_give(&evt_sem);
}

#if IS_ENABLED(CONFIG_UBLOX_MAX7Q)
static void gps_trigger_handler(const struct device *dev,
		const struct sensor_trigger *trig)
{
	struct app_evt_t *ev;

	LOG_INF("GPS trigger handler");

	/** Disable NMEA trigger after successful location fix */
	nmea_trigger_enable(GPS_TRIG_DISABLE);

	ev = app_evt_alloc();
	ev->event_type = EV_GPS_FIX;
	app_evt_put(ev);
	k_sem_give(&evt_sem);
}
#endif

int init_leds(void)
{
	int err = 0;

	if (!led_green.port) {
		LOG_INF("Green LED not available");
	} else if (!device_is_ready(led_green.port)) {
		LOG_ERR("Green LED device not ready");
		led_green.port = NULL;
		err = -ENODEV;
	} else {
		/* Init green led as output and turn it on boot */
		err = gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("failed to configure Green LED gpio: %d", err);
			led_green.port = NULL;
		}
	}

	if (!led_blue.port) {
		LOG_INF("Blue LED not available");
	} else if (!device_is_ready(led_blue.port)) {
		LOG_ERR("Blue LED device not ready");
		led_blue.port = NULL;
		err = -ENODEV;
	} else {
		/* Init blue led as output and turn it on boot */
		err = gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_ACTIVE);
		if (err) {
			LOG_ERR("failed to configure Blue LED gpio: %d", err);
			led_blue.port = NULL;
		}
	}

	return err;
}

void led_enable(const struct gpio_dt_spec *led, int enable) {
	if (led->port) {
		gpio_pin_set_dt(led, enable);
	}
}

static void dl_callback(uint8_t port, bool data_pending,
			int16_t rssi, int8_t snr,
			uint8_t len, const uint8_t *data)
{
	LOG_INF("Port %d, Pending %d, RSSI %ddB, SNR %ddBm", port, data_pending, rssi, snr);
	if (data) {
		LOG_HEXDUMP_INF(data, len, "Payload: ");
#if IS_ENABLED(CONFIG_SHELL)
		dl_shell_cmd_exec(len, data);
#endif
	}
}

struct lorawan_downlink_cb downlink_cb = {
	.port = LW_RECV_PORT_ANY,
	.cb = dl_callback
};

static void lorwan_datarate_changed(enum lorawan_datarate dr)
{
	uint8_t unused, max_size;

	lorawan_get_payload_sizes(&unused, &max_size);
	LOG_INF("New Datarate: DR_%d, Max Payload %d", dr, max_size);
}

static const enum sensor_channel channels[] = {
	SENSOR_CHAN_ACCEL_X,
	SENSOR_CHAN_ACCEL_Y,
	SENSOR_CHAN_ACCEL_Z,
};

struct s_accel_values {
	struct sensor_value val;
	const char *sign;
};

static int print_accels(const struct device *dev)
{
	int err;
	struct s_accel_values accel[3] = {
		{.sign = ""},
		{.sign = ""},
		{.sign = ""}
	};

	err = sensor_sample_fetch(dev);
	if (err < 0) {
		LOG_ERR("%s: sensor_sample_fetch() failed: %d", dev->name, err);
		return err;
	}

	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		err = sensor_channel_get(dev, channels[i], &accel[i].val);
		if (err < 0) {
			LOG_ERR("%s: sensor_channel_get(%c) failed: %d\n",
					dev->name, 'X' + i, err);
			return err;
		}
		if ((accel[i].val.val1 < 0) || (accel[i].val.val2 < 0)) {
			accel[i].sign = "-";
			accel[i].val.val1 = abs(accel[i].val.val1);
			accel[i].val.val2 = abs(accel[i].val.val2);
		}
	}

	LOG_INF("%s: %d, %s%d.%06d, %s%d.%06d, %s%d.%06d (m/s^2)",
		dev->name, lorawan_status.acc_events,
		accel[0].sign, accel[0].val.val1, accel[0].val.val2,
		accel[1].sign, accel[1].val.val1, accel[1].val.val2,
		accel[2].sign, accel[2].val.val1, accel[2].val.val2);

	return 0;
}

#ifdef CONFIG_LIS2DH_TRIGGER
static void trigger_handler(const struct device *dev,
		const struct sensor_trigger *trig)
{
	struct app_evt_t *ev;

	LOG_INF("ACC trigger handler");

	/* bounce very "touchy" accell sensor */
	if ((k_uptime_get_32() - lorawan_status.last_accel_event) < 5000) {
		return;
	}
	lorawan_status.last_accel_event = k_uptime_get_32();
	lorawan_status.acc_events++;

	ev = app_evt_alloc();
	ev->event_type = EV_ACC;
	app_evt_put(ev);
	k_sem_give(&evt_sem);
}
#endif

#if 0
int init_accel(struct s_helium_mapper_ctx *ctx)
{
	const struct device *accel_dev;
	int err = 0;

	accel_dev = DEVICE_DT_GET(DT_ALIAS(accel0));
	if (!device_is_ready(accel_dev)) {
		LOG_ERR("%s: device not ready.", accel_dev->name);
		return -ENODEV;
	}

	ctx->accel_dev = accel_dev;

	print_accels(accel_dev);

#if CONFIG_LIS2DH_TRIGGER
	struct sensor_trigger trig;
	enum sensor_channel chan = SENSOR_CHAN_ACCEL_XYZ;

	if (IS_ENABLED(CONFIG_LIS2DH_ODR_RUNTIME)) {
		struct sensor_value attr = {
			.val1 = 10,
			.val2 = 0,
		};

		err = sensor_attr_set(accel_dev, chan,
				SENSOR_ATTR_SAMPLING_FREQUENCY,
				&attr);
		if (err != 0) {
			LOG_ERR("Failed to set odr: %d", err);
			return err;
		}
		LOG_INF("Sampling at %u Hz", attr.val1);

		/* set slope threshold to 30 dps */
		sensor_degrees_to_rad(30, &attr); /* convert to rad/s */

		if (sensor_attr_set(accel_dev, chan,
					SENSOR_ATTR_SLOPE_TH, &attr) < 0) {
			LOG_ERR("Accel: cannot set slope threshold.\n");
			return err;
		}

		/* set slope duration to 4 samples */
		attr.val1 = 4;
		attr.val2 = 0;

		if (sensor_attr_set(accel_dev, chan,
					SENSOR_ATTR_SLOPE_DUR, &attr) < 0) {
			LOG_ERR("Accel: cannot set slope duration.\n");
			return err;
		}

#if CONFIG_LIS2DH_ACCEL_HP_FILTERS
		/* Set High Pass filter for int 1 */
		attr.val1 = 1U;
		attr.val2 = 0;
		if (sensor_attr_set(accel_dev, chan,
					SENSOR_ATTR_CONFIGURATION, &attr) < 0) {
			LOG_ERR("Accel: cannot set high pass filter for int 1.");
			return err;
		}
#endif
	}

	trig.type = SENSOR_TRIG_DELTA;
	trig.chan = chan;

	err = sensor_trigger_set(accel_dev, &trig, trigger_handler);
	if (err != 0) {
		LOG_ERR("Failed to set trigger: %d", err);
	}
#endif
	return err;
}
#endif

static const char *lorawan_state_str(enum lorawan_state_e state)
{
	switch(state) {
	case NOT_JOINED:
		return "NOT_JOINED";
	case JOINED:
		return "JOINED";
	}

	return "UNKNOWN";
}

void lorawan_state(struct s_helium_mapper_ctx *ctx, enum lorawan_state_e state)
{
	uint32_t join_try_interval_sec = lorawan_config.join_try_interval;

	LOG_INF("LoraWAN state set to: %s", lorawan_state_str(state));

	switch (state) {
	case NOT_JOINED:
		if (!lorawan_config.auto_join) {
			LOG_WRN("Join is not enabled");
			break;
		}
		/* Turn green led on to indicate not joined state */
		led_enable(&led_green, 1);

		lorawan_status.joined = false;
		LOG_INF("Lora join timer start for %d sec", join_try_interval_sec);
		k_timer_start(&ctx->lora_join_timer, K_SECONDS(join_try_interval_sec),
				K_NO_WAIT);
		k_sem_give(&ctx->lora_join_sem);
		break;

	case JOINED:
		/* Turn green led off on join success */
		led_enable(&led_green, 0);

		lorawan_status.joined = true;
		lorawan_status.join_retry_sessions_count = 0;
		LOG_INF("Stop Lora join retry timer");
		k_timer_stop(&ctx->lora_join_timer);
		break;

	default:
		LOG_ERR("Unknown LoraWAN state");
		break;
	} /* switch */
}

static void lora_join_timer_handler(struct k_timer *timer)
{
	struct s_helium_mapper_ctx *ctx =
		CONTAINER_OF(timer, struct s_helium_mapper_ctx, lora_join_timer);

	LOG_INF("LoraWAN join timer handler");

	/* If not joined within 'join_try_interval', try again */
	if (!lorawan_status.joined) {
		lorawan_state(ctx, NOT_JOINED);
	}
}

int join_lora(struct s_helium_mapper_ctx *ctx) {
	struct lorawan_join_config join_cfg;
	int retry = lorawan_config.join_try_count;
	int ret = 0;

	join_cfg.mode = lorawan_config.lora_mode;
	join_cfg.dev_eui = lorawan_config.dev_eui;
	join_cfg.otaa.join_eui = lorawan_config.app_eui;
	join_cfg.otaa.app_key = lorawan_config.app_key;
	join_cfg.otaa.nwk_key = lorawan_config.app_key;

	if (lorawan_config.auto_join) {
		while (retry--) {
			LOG_INF("Joining network over OTAA. Attempt: %d",
					lorawan_config.join_try_count - retry);
			ret = lorawan_join(&join_cfg);
			if (ret == 0) {
				break;
			}
			LOG_ERR("lorawan_join_network failed: %d", ret);
			k_sleep(K_SECONDS(15));
		}

		if (ret == 0) {
			lorawan_state(ctx, JOINED);
		}
	}

	return ret;
}

static void lora_join_thread(struct s_helium_mapper_ctx *ctx) {
	uint16_t retry_count_conf = lorawan_config.max_join_retry_sessions_count;
	int err;

	while (1) {
		k_sem_take(&ctx->lora_join_sem, K_FOREVER);
		err = join_lora(ctx);
		if (err) {
			lorawan_status.join_retry_sessions_count++;
		}

		if (lorawan_status.join_retry_sessions_count > retry_count_conf) {
			LOG_ERR("Reboot in 30sec");
			k_sleep(K_SECONDS(30));
			sys_reboot(SYS_REBOOT_WARM);
			return; /* won't reach this */
		}
	}
}

int init_lora(struct s_helium_mapper_ctx *ctx) {
	const struct device *lora_dev;
	int ret;

	lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));
	if (!device_is_ready(lora_dev)) {
		LOG_ERR("%s: device not ready.", lora_dev->name);
		return -ENODEV;
	}

	ret = lorawan_start();
	if (ret < 0) {
		LOG_ERR("lorawan_start failed: %d", ret);
		return ret;
	}

	lorawan_register_downlink_callback(&downlink_cb);
	lorawan_register_dr_changed_callback(lorwan_datarate_changed);
	lorawan_set_datarate(lorawan_config.data_rate);

	k_sem_init(&ctx->lora_join_sem, 0, K_SEM_MAX_LIMIT);

	k_thread_create(&ctx->thread, lora_join_thread_stack,
			K_THREAD_STACK_SIZEOF(lora_join_thread_stack),
			(k_thread_entry_t)lora_join_thread, ctx, NULL, NULL,
			K_PRIO_PREEMPT(LORA_JOIN_THREAD_PRIORITY), 0,
			K_SECONDS(1));

	k_thread_name_set(&ctx->thread, "lora_join");

	/* make initial join */
	lorawan_state(ctx, NOT_JOINED);

	return 0;
}

void init_timers(struct s_helium_mapper_ctx *ctx)
{
	k_timer_init(&ctx->send_timer, send_timer_handler, NULL);
	k_timer_init(&ctx->delayed_timer, delayed_timer_handler, NULL);
	k_timer_init(&ctx->gps_off_timer, gps_off_timer_handler, NULL);
	k_timer_init(&ctx->lora_join_timer, lora_join_timer_handler, NULL);

	update_send_timer(ctx);
}

void send_event(struct s_helium_mapper_ctx *ctx) {
	time_t min_delay = lorawan_config.send_min_delay * 1000;
	time_t last_pos_send = lorawan_status.last_pos_send;
	struct app_evt_t *ev;

	if (!lorawan_status.joined) {
		LOG_WRN("Not joined");
		return;
	}

	if (!lorawan_config.send_repeat_time) {
		LOG_WRN("Periodic send is disabled");
		return;
	}

	if (lorawan_status.delayed_active) {
		time_t time_left = k_timer_remaining_get(&ctx->delayed_timer);
		LOG_INF("Delayed timer already active, %lld sec left",
				time_left / 1000);
		return;
	}

	if ((k_uptime_get_32() - last_pos_send) > min_delay) {
		/* Enable NMEA trigger and wait for location fix */
		ev = app_evt_alloc();
		ev->event_type = EV_NMEA_TRIG_ENABLE;
		app_evt_put(ev);
		k_sem_give(&evt_sem);
	} else {
		time_t now_ms = k_uptime_get_32();
		time_t wait_time =
			abs(min_delay - (now_ms - last_pos_send) >= 0)
			? (min_delay - (now_ms - last_pos_send)) : min_delay;

		LOG_INF("Delayed timer start for %lld sec", wait_time / 1000);
		k_timer_start(&ctx->delayed_timer, K_MSEC(wait_time), K_NO_WAIT);
		lorawan_status.delayed_active = true;
	}
}

void lora_send_msg(struct s_helium_mapper_ctx *ctx)
{
	int64_t last_pos_send_ok_sec;
	int64_t delta_sent_ok_sec;
	uint8_t msg_type = lorawan_config.confirmed_msg;
	uint32_t inactive_time_window_sec = lorawan_config.max_inactive_time_window;
	uint32_t max_failed_msgs = lorawan_config.max_failed_msg;
	int err;

	if (!lorawan_status.joined) {
		LOG_WRN("Not joined");
		return;
	}

	memset(data_ptr, 0, sizeof(struct s_mapper_data));

	mapper_data.fix = ctx->gps_fix ? 1 : 0;

#if IS_ENABLED(CONFIG_ADC)
	int batt_mV;
	err = read_battery(&batt_mV);
	if (err == 0) {
		mapper_data.battery = (uint16_t)batt_mV;
	}
#endif

#if IS_ENABLED(CONFIG_UBLOX_MAX7Q)
	read_location(&mapper_data);
#endif

	LOG_HEXDUMP_DBG(data_ptr, sizeof(struct s_mapper_data),
			"mapper_data");

	/* Send at least one confirmed msg on every 10 to check connectivity */
	if (msg_type == LORAWAN_MSG_UNCONFIRMED &&
			!(lorawan_status.msgs_sent % 10)) {
		msg_type = LORAWAN_MSG_CONFIRMED;
	}

	LOG_INF("Lora send -------------->");

	led_enable(&led_blue, 1);
	err = lorawan_send(lorawan_config.app_port,
			data_ptr, sizeof(struct s_mapper_data),
			msg_type);
	if (err < 0) {
		//TODO: make special LED pattern in this case
		lorawan_status.msgs_failed++;
		lorawan_status.msgs_failed_total++;
		LOG_ERR("lorawan_send failed: %d", err);
	} else {
		lorawan_status.msgs_sent++;
		lorawan_status.msgs_failed = 0;
		/* Remember last successfuly send message time */
		lorawan_status.last_pos_send_ok = k_uptime_get();
		LOG_INF("Data sent!");
	}
	led_enable(&led_blue, 0);

	/* Remember last send time */
	lorawan_status.last_pos_send = k_uptime_get();

	ctx->gps_fix = false;

	last_pos_send_ok_sec = lorawan_status.last_pos_send_ok;
	delta_sent_ok_sec = k_uptime_delta(&last_pos_send_ok_sec) / 1000;
	LOG_INF("delta_sent_ok_sec: %lld", delta_sent_ok_sec);

	if (lorawan_status.msgs_failed > max_failed_msgs ||
			delta_sent_ok_sec > inactive_time_window_sec) {
		LOG_ERR("Too many failed msgs: Try to re-join.");
		lorawan_state(ctx, NOT_JOINED);
		k_sem_give(&ctx->lora_join_sem);
	}
}

#if IS_ENABLED(CONFIG_SHELL)
void shell_cb(enum shell_cmd_event event, void *data) {
	struct s_helium_mapper_ctx *ctx = (struct s_helium_mapper_ctx *)data;

	switch (event) {
	case SHELL_CMD_SEND_TIMER:
		update_send_timer(ctx);
		break;
	case SHELL_CMD_SEND_TIMER_GET:
		time_t time_st_left = k_timer_remaining_get(&ctx->send_timer);
		LOG_INF("Send timer %lld sec left", time_st_left / 1000);
		break;
	default:
		LOG_WRN("Unknown shell cmd event");
		break;
	} /* switch */
}
#endif

void app_evt_handler(struct app_evt_t *ev, struct s_helium_mapper_ctx *ctx)
{
	switch (ev->event_type) {
	case EV_TIMER:
		LOG_INF("Event Timer");
		send_event(ctx);
		break;

	case EV_ACC:
		LOG_INF("Event ACC");
		print_accels(ctx->accel_dev);
		send_event(ctx);
		break;

	case EV_NMEA_TRIG_ENABLE:
		LOG_INF("Event NMEA_TRIG_ENABLE");
#if IS_ENABLED(CONFIG_UBLOX_MAX7Q)
		nmea_trigger_enable(GPS_TRIG_ENABLE);
#endif
		update_gps_off_timer(ctx);
		break;

	case EV_NMEA_TRIG_DISABLE:
		LOG_INF("Event NMEA_TRIG_DISABLE");
#if IS_ENABLED(CONFIG_UBLOX_MAX7Q)
		nmea_trigger_enable(GPS_TRIG_DISABLE);
#endif
		/* If we aren't able to get gps fix during the whole
		   GPS ON Interval, send lora message with other telemetry
		   data and old position data if available.
		*/
		if (!ctx->gps_fix) {
			lora_send_msg(ctx);
		}
		break;

	case EV_GPS_FIX:
		LOG_INF("Event GPS_FIX");
		ctx->gps_fix = true;
		lora_send_msg(ctx);
		break;

	default:
		LOG_ERR("Unknown event");
		break;

	} /* switch */
}

int main(void)
{
	struct s_helium_mapper_ctx *ctx = &g_ctx;
	struct app_evt_t *ev;
	int ret;

	ret = init_leds();
	if (ret) {
		return ret;
	}

#if IS_ENABLED(CONFIG_SETTINGS)
	ret = load_config();
	if (ret) {
		goto fail;
	}
#endif

	init_timers(ctx);

#if 0
	ret = init_accel(ctx);
	if (ret) {
		goto fail;
	}
#endif

#if IS_ENABLED(CONFIG_UBLOX_MAX7Q)
	ret = init_gps();
	if (ret) {
		goto fail;
	}

	ret = gps_set_trigger_handler(gps_trigger_handler);
	if (ret) {
		goto fail;
	}
#endif

#if IS_ENABLED(CONFIG_SHELL)
	ret = init_shell();
	if (ret) {
		goto fail;
	}

	shell_register_cb(shell_cb, ctx);
#endif

	ret = init_lora(ctx);
	if (ret) {
		LOG_ERR("Rebooting in 30 sec.");
		k_sleep(K_SECONDS(30));
		sys_reboot(SYS_REBOOT_WARM);
		goto fail;
	}

	while (true) {
		LOG_INF("Waiting for events...");

		k_sem_take(&evt_sem, K_FOREVER);

		while ((ev = app_evt_get()) != NULL) {
			app_evt_handler(ev, ctx);
			app_evt_free(ev);
		}
	}

fail:
	while (true) {
		if (led_blue.port) {
			gpio_pin_set_dt(&led_blue, 0);
			k_sleep(K_MSEC(250));
			gpio_pin_set_dt(&led_blue, 1);
		}
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
