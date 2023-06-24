
#include <stdio.h>
#include <zephyr/settings/settings.h>

#include "lorawan_config.h"

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(helium_mapper_nvm);

#define HELIUM_MAPPER_SETTINGS_BASE "helium_mapper/nvm"

struct hm_nvm_setting_descr {
	const char *name;
	const char *setting_name;
	size_t size;
	off_t offset;
};

#define HM_NVM_SETTING_DESCR(_member) \
	{									\
		.name = STRINGIFY(_member),					\
		.setting_name =							\
			HELIUM_MAPPER_SETTINGS_BASE "/" STRINGIFY(_member),	\
		.offset = offsetof(struct s_lorawan_config, _member),			\
		.size = sizeof(((struct s_lorawan_config *)0)->_member),		\
	}

static const struct hm_nvm_setting_descr hm_nvm_setting_descriptors[] = {
	HM_NVM_SETTING_DESCR(dev_eui),
	HM_NVM_SETTING_DESCR(app_eui),
	HM_NVM_SETTING_DESCR(app_key),
	HM_NVM_SETTING_DESCR(confirmed_msg),
	HM_NVM_SETTING_DESCR(auto_join),
	HM_NVM_SETTING_DESCR(send_repeat_time),
	HM_NVM_SETTING_DESCR(send_min_delay),
	HM_NVM_SETTING_DESCR(max_gps_on_time),
};

void hm_lorawan_nvm_save_settings(const char *name)
{
	LOG_DBG("Saving LoRaWAN settings");

	struct s_lorawan_config *nvm = (void *)&lorawan_config;

	for (uint32_t i = 0; i < ARRAY_SIZE(hm_nvm_setting_descriptors); i++) {
		const struct hm_nvm_setting_descr *descr =
			&hm_nvm_setting_descriptors[i];

		if (!strncmp(descr->name, name, strlen(name))) {
			LOG_DBG("Saving configuration %s", descr->setting_name);
			int err = settings_save_one(descr->setting_name,
						(char *)nvm + descr->offset,
						descr->size);
			if (err) {
				LOG_ERR("Could not save settings %s, err: %d",
					descr->name, err);
			}
		}
	}
}

static int hm_load_setting(void *tgt, size_t tgt_size,
			const char *key, size_t len,
			settings_read_cb read_cb, void *cb_arg)
{
	if (len != tgt_size) {
		LOG_ERR("Can't load '%s' state, size mismatch.",
			key);
		return -EINVAL;
	}

	if (!tgt) {
		LOG_ERR("Can't load '%s' state, no target.",
			key);
		return -EINVAL;
	}

	if (read_cb(cb_arg, tgt, len) != len) {
		LOG_ERR("Can't load '%s' state, short read.",
			key);
		return -EINVAL;
	}

	return 0;
}

static int hm_on_setting_loaded(const char *key, size_t len,
			   settings_read_cb read_cb,
			   void *cb_arg, void *param)
{
	int err = 0;
	struct s_lorawan_config *nvm = param;

	LOG_DBG("Key: %s", key);

	for (uint32_t i = 0; i < ARRAY_SIZE(hm_nvm_setting_descriptors); i++) {
		const struct hm_nvm_setting_descr *descr =
			&hm_nvm_setting_descriptors[i];

		LOG_DBG("key: %s, desc->name: %s", key, descr->name);
		if (strcmp(descr->name, key) == 0) {
			err = hm_load_setting((char *)nvm + descr->offset,
				descr->size, key, len, read_cb, cb_arg);
			if (err) {
				LOG_ERR("Could not read setting %s", descr->name);
			}
			return err;
		}
	}

	LOG_WRN("Unknown LoRaWAN setting: %s", key);

	return err;
}

int config_nvm_data_restore(void)
{
	int err = 0;

	LOG_DBG("Restoring helium_mapper config settings");

	err = settings_load_subtree_direct(HELIUM_MAPPER_SETTINGS_BASE,
					   hm_on_setting_loaded,
					   (void *)&lorawan_config);
	if (err) {
		LOG_ERR("Could not load config settings, err %d", err);
		return err;
	}

	LOG_DBG("config setings restored");

	return 0;
}

int load_config(void)
{
	int err = 0;

	if (!IS_ENABLED(CONFIG_LORAWAN_NVM_NONE)) {
		err = settings_subsys_init();
		if (err) {
			LOG_ERR("Could not init settings, err: %d", err);
			return err;
		}
		err = config_nvm_data_restore();
		if (err) {
			LOG_ERR("Could not restore settings, err: %d", err);
			return err;
		}
	}

	return err;
}

