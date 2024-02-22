/*
 *
 */

#ifndef __LORAWAN_CONFIG_H__
#define __LORAWAN_CONFIG_H__

#include <stdio.h>
#include <zephyr/lorawan/lorawan.h>

struct s_lorawan_config
{
	/* OTAA Device EUI MSB */
	char dev_eui[8];
	/* OTAA Application EUI MSB */
	uint8_t app_eui[8];
	/* OTAA Application Key MSB */
	uint8_t app_key[16];
	/* LoRaWAN activation mode */
	uint8_t lora_mode;
	/* Data rate (depnends on Region) */
	uint8_t data_rate;
	/* LoRaWAN class */
	uint8_t lora_class;
	/* Type of messages: confirmed or un-confirmed */
	uint8_t confirmed_msg;
	/* Data port to send data */
	uint8_t app_port;
	/* Flag if node joins automatically after reboot */
	bool auto_join;
	/* Send repeat time in seconds */
	uint32_t send_repeat_time;
	/* Max attempt to join network */
	uint8_t join_try_count;
	/* Max LoraWAN join sessions retry count before reboot */
	uint16_t max_join_retry_sessions_count;
	/* Max LoraWAN join window interval in seconds */
	uint32_t join_try_interval;
	/* Max time window of no ack'd msg received before re-join in seconds */
	uint32_t max_inactive_time_window;
	/* Number of failed message before re-join */
	uint32_t max_failed_msg;
};

extern struct s_lorawan_config lorawan_config;

struct s_meteo_data
{
	uint32_t temp_mK;
	uint32_t pressure_Pa;
	uint8_t humidity_percent;
	uint16_t battery_mV;
} __packed;

struct s_status {
	bool joined;
	bool delayed_active;
	uint32_t msgs_sent;
	uint32_t msgs_failed;
	uint32_t msgs_failed_total;
	uint16_t join_retry_sessions_count;
};

extern struct s_status lorawan_status;

#endif /* __LORAWAN_CONFIG_H__ */
