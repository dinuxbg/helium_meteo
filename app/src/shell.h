/*
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __HELIUM_MAPPER_SHELL_H__
#define __HELIUM_MAPPER_SHELL_H__

enum shell_cmd_event {
	SHELL_CMD_SEND_TIMER,
	SHELL_CMD_SEND_TIMER_GET,
};

#define DL_SHELL_CMD_BUF_SIZE 64
#define DL_SHELL_CMD_PREFIX "shell "
void dl_shell_cmd_exec(uint8_t len, const uint8_t *data);

typedef void (*shell_cmd_cb_t)(enum shell_cmd_event event, void *user_data);

int init_shell(void);
void shell_register_cb(shell_cmd_cb_t cb, void *);

#endif /* __HELIUM_MAPPER_SHELL_H__ */
