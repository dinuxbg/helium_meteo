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
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>


#include "lorawan_config.h"
#if IS_ENABLED(CONFIG_ADC)
#include "battery.h"
#endif
#include "nvm.h"
#if IS_ENABLED(CONFIG_SHELL)
#include "shell.h"
#endif

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(helium_meteo);

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec dt_led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec dt_sw0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback user_button_cb_data;

const struct device *const dev_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));


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
	.send_repeat_time = 3600 / 2,
	/* max join attempt in one join session */
	.join_try_count = 5,
	/* max join sessions before give up and reboot. 20 * 5 = 100 join attempts */
	.max_join_retry_sessions_count = 20,
	/* max join session interval in sec */
	.join_try_interval = 300,
	.max_inactive_time_window = 2 * 3600,
	.max_failed_msg = 120,
};

struct s_status lorawan_status = {
	.joined = false,
	.msgs_sent = 0,
	.msgs_failed = 0,
	.msgs_failed_total = 0,
	.join_retry_sessions_count = 0,
};

struct s_meteo_data meteo_data;
char *data_ptr = (char*)&meteo_data;

#define LORA_JOIN_THREAD_STACK_SIZE 1500
#define LORA_JOIN_THREAD_PRIORITY 10
K_KERNEL_STACK_MEMBER(lora_join_thread_stack, LORA_JOIN_THREAD_STACK_SIZE);

struct s_helium_meteo_ctx {
	const struct device *lora_dev;
	const struct device *meteo_dev;
	struct k_timer send_timer;
	struct k_timer lora_join_timer;
	struct k_thread thread;
	struct k_sem lora_join_sem;
};

struct s_helium_meteo_ctx g_ctx;

enum lorawan_state_e {
	NOT_JOINED,
	JOINED,
};

/* Event FIFO */

K_FIFO_DEFINE(evt_fifo);

enum evt_t {
	EV_TIMER,
	EV_BUTTON,
	EV_SEND_DATA,
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


void update_send_timer(struct s_helium_meteo_ctx *ctx)
{
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

void user_button_pressed(const struct device *dev, struct gpio_callback *cb,
                    uint32_t pins)
{
	struct app_evt_t *ev;

	LOG_INF("Button handler");

	ev = app_evt_alloc();
	ev->event_type = EV_BUTTON;
	app_evt_put(ev);
	k_sem_give(&evt_sem);
}


int init_buttons(void)
{
	int err;

	if (!dt_sw0.port) {
		LOG_INF("User button not available");
		return -ENODEV;
	}
	if (!device_is_ready(dt_sw0.port)) {
		LOG_ERR("User button device not ready");
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&dt_sw0, GPIO_INPUT);
	if (err) {
		LOG_ERR("failed to configure user button gpio: %d", err);
		return err;
	}
	err = gpio_pin_interrupt_configure_dt(&dt_sw0, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("failed to configure user button gpio: %d", err);
		return err;
	}
	gpio_init_callback(&user_button_cb_data, user_button_pressed, BIT(dt_sw0.pin));
	gpio_add_callback(dt_sw0.port, &user_button_cb_data);

	return err;
}

int init_leds(void)
{
	int err = 0;

	if (!dt_led0.port) {
		LOG_INF("LED0 not available");
		return -ENODEV;
	}
	if (!device_is_ready(dt_led0.port)) {
		LOG_ERR("LED0 device not ready");
		return -ENODEV;
	}
	/* Init led as output and turn it off */
	err = gpio_pin_configure_dt(&dt_led0, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("failed to configure LED0 gpio: %d", err);
		return err;
	}
	/* This is required to reach low-power modes. */
	err = gpio_pin_configure_dt(&dt_led0, GPIO_DISCONNECTED);

	return err;
}

void led_enable(const struct gpio_dt_spec *led, int enable)
{
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

static int init_meteo(struct s_helium_meteo_ctx *ctx)
{
        ctx->meteo_dev = DEVICE_DT_GET_ANY(bosch_bme280);

        if (ctx->meteo_dev == NULL) {
                /* No such node, or the node does not have status "okay". */
                LOG_ERR("No BME280 device found.\n");
                return -ENODEV;
        }

        if (!device_is_ready(ctx->meteo_dev)) {
                LOG_ERR("Device \"%s\" is not ready; "
                       "check the driver initialization logs for errors.\n",
                       ctx->meteo_dev->name);
                return -EBUSY;
        }

        LOG_INF("Found device \"%s\", getting sensor data\n", ctx->meteo_dev->name);

        return 0;
}

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

void lorawan_state(struct s_helium_meteo_ctx *ctx, enum lorawan_state_e state)
{
	uint32_t join_try_interval_sec = lorawan_config.join_try_interval;

	LOG_INF("LoraWAN state set to: %s", lorawan_state_str(state));

	switch (state) {
	case NOT_JOINED:
		if (!lorawan_config.auto_join) {
			LOG_WRN("Join is not enabled");
			break;
		}
		lorawan_status.joined = false;
		LOG_INF("Lora join timer start for %d sec", join_try_interval_sec);
		k_timer_start(&ctx->lora_join_timer, K_SECONDS(join_try_interval_sec),
				K_NO_WAIT);
		k_sem_give(&ctx->lora_join_sem);
		break;

	case JOINED:
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
	struct s_helium_meteo_ctx *ctx =
		CONTAINER_OF(timer, struct s_helium_meteo_ctx, lora_join_timer);

	LOG_INF("LoraWAN join timer handler");

	/* If not joined within 'join_try_interval', try again */
	if (!lorawan_status.joined) {
		lorawan_state(ctx, NOT_JOINED);
	}
}

int join_lora(struct s_helium_meteo_ctx *ctx)
{
	struct pm_policy_latency_request req;
	struct lorawan_join_config join_cfg;
	int retry = lorawan_config.join_try_count;
	int ret = 0;

	pm_policy_latency_request_add(&req, 3);

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

	pm_policy_latency_request_remove(&req);

	return ret;
}

static void lora_join_thread(struct s_helium_meteo_ctx *ctx)
{
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

int init_lora(struct s_helium_meteo_ctx *ctx)
{
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

void init_timers(struct s_helium_meteo_ctx *ctx)
{
	k_timer_init(&ctx->send_timer, send_timer_handler, NULL);
	k_timer_init(&ctx->lora_join_timer, lora_join_timer_handler, NULL);

	update_send_timer(ctx);
}

void send_event(struct s_helium_meteo_ctx *ctx)
{
	struct app_evt_t *ev;

	if (!lorawan_status.joined) {
		LOG_WRN("Not joined");
		return;
	}

	if (!lorawan_config.send_repeat_time) {
		LOG_WRN("Periodic send is disabled");
		return;
	}

	ev = app_evt_alloc();
	ev->event_type = EV_SEND_DATA;
	app_evt_put(ev);
	k_sem_give(&evt_sem);
}

void lora_send_msg(struct s_helium_meteo_ctx *ctx)
{
	struct pm_policy_latency_request req;
	uint8_t msg_type = lorawan_config.confirmed_msg;
	uint32_t max_failed_msgs = lorawan_config.max_failed_msg;
	int err;

	if (!lorawan_status.joined) {
		LOG_WRN("Not joined");
		return;
	}

	pm_policy_latency_request_add(&req, 3);

	memset(data_ptr, 0, sizeof(struct s_meteo_data));

	if (ctx->meteo_dev != NULL) {
		struct sensor_value temperature, press, humidity;

		err = sensor_sample_fetch(ctx->meteo_dev);
		if (err != 0)
			LOG_ERR("sensor_sample_fetch failed: %d", err);

		err = sensor_channel_get(ctx->meteo_dev, SENSOR_CHAN_AMBIENT_TEMP, &temperature);
		if (err != 0)
			LOG_ERR("get temperature failed: %d", err);
		err = sensor_channel_get(ctx->meteo_dev, SENSOR_CHAN_PRESS, &press);
		if (err != 0)
			LOG_ERR("get pressure failed: %d", err);
		err = sensor_channel_get(ctx->meteo_dev, SENSOR_CHAN_HUMIDITY, &humidity);
		if (err != 0)
			LOG_ERR("get humidity failed: %d", err);

		LOG_INF("meteo: %d Cel ; %d %%RH\n", temperature.val1, humidity.val1);

		/* Celsius to milliKelvin. */
		meteo_data.temp_mK = (temperature.val1 + 273) * 1000 + (temperature.val2 / 1000 + 150);
		/* KPa to Pa */
		meteo_data.pressure_Pa = press.val1 * 1000;
		/* Both Zephyr and Helium Meteo in percents. */
		meteo_data.humidity_percent = humidity.val1;
	}

#if IS_ENABLED(CONFIG_ADC)
	int batt_mV;
	err = read_battery(&batt_mV);
	if (err == 0) {
		meteo_data.battery_mV = (uint16_t)batt_mV;
	}
#endif

	LOG_HEXDUMP_DBG(data_ptr, sizeof(struct s_meteo_data),
			"meteo_data");

	/* Send at least one confirmed msg on every 10 to check connectivity */
	if (msg_type == LORAWAN_MSG_UNCONFIRMED &&
			!(lorawan_status.msgs_sent % 10)) {
		msg_type = LORAWAN_MSG_CONFIRMED;
	}

	LOG_INF("Lora send -------------->");

	led_enable(&dt_led0, 1);
	err = lorawan_send(lorawan_config.app_port,
			data_ptr, sizeof(struct s_meteo_data),
			msg_type);
	if (err < 0) {
		//TODO: make special LED pattern in this case
		lorawan_status.msgs_failed++;
		lorawan_status.msgs_failed_total++;
		LOG_ERR("lorawan_send failed: %d", err);
	} else {
		lorawan_status.msgs_sent++;
		lorawan_status.msgs_failed = 0;
		LOG_INF("Data sent!");
	}
	led_enable(&dt_led0, 0);

	if (lorawan_status.msgs_failed > max_failed_msgs) {
		LOG_ERR("Too many failed msgs: Try to re-join.");
		lorawan_state(ctx, NOT_JOINED);
		k_sem_give(&ctx->lora_join_sem);
	}
	pm_policy_latency_request_remove(&req);
}

#if IS_ENABLED(CONFIG_SHELL)
void shell_cb(enum shell_cmd_event event, void *data) {
	struct s_helium_meteo_ctx *ctx = (struct s_helium_meteo_ctx *)data;

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

void app_evt_handler(struct app_evt_t *ev, struct s_helium_meteo_ctx *ctx)
{
	switch (ev->event_type) {
	case EV_TIMER:
		LOG_INF("Event Timer");
		send_event(ctx);
		update_send_timer(ctx);
		break;

	case EV_BUTTON:
		LOG_INF("Event Button");
		send_event(ctx);
		break;

	case EV_SEND_DATA:
		lora_send_msg(ctx);
		break;
	default:
		LOG_ERR("Unknown event");
		break;

	} /* switch */
}

int main(void)
{
	struct s_helium_meteo_ctx *ctx = &g_ctx;
	struct app_evt_t *ev;
	int ret;

	ret = init_leds();
	if (ret) {
		return ret;
	}

	ret = init_buttons();
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

	ret = init_meteo(ctx);
	if (ret) {
		goto fail;
	}

#if IS_ENABLED(CONFIG_SHELL)
	if (device_is_ready(dev_console) && pm_device_wakeup_is_capable(dev_console)) {
		ret = pm_device_wakeup_enable(dev_console, true);
		if (!ret) {
			printk("Could not enable wakeup source\n");
		} else {
			printk("Wakeup source enable ok\n");
		}
	}

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
		if (dt_led0.port) {
			gpio_pin_set_dt(&dt_led0, 0);
			k_sleep(K_MSEC(250));
			gpio_pin_set_dt(&dt_led0, 1);
		}
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
