/*
 * config.h - config.json loading and atomic task saving.
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 */
#ifndef T410DASH_CONFIG_H
#define T410DASH_CONFIG_H

#include <limits.h>
#include <stddef.h>

#define TASK_MAX 256

typedef struct {
	char account_number[64];
	char balance_eur[32];
	char toggle_key[8];

	char countdown_label[64];
	char countdown_date[16];   /* YYYY-MM-DD */

	double latitude, longitude;
	char city[64];

	double high_temp_c;
	int shutdown_delay_sec;
	int snooze_sec;

	char task[TASK_MAX];
} config;

/* Pick the config path: explicit argument, ./config.json next to the
 * binary, or ~/.config/t410dash/config.json (in that order). */
void config_resolve_path(const char *arg, const char *argv0, char *out, size_t outlen);

/* Load config from path; missing file or fields fall back to defaults.
 * Returns 0 if the file was read and parsed, -1 if defaults were used. */
int config_load(const char *path, config *cfg);

/* Update only the "task" field, keeping every other field. Writes a temp
 * file in the same directory and renames it into place. Returns 0 on success. */
int config_save_task(const char *path, const char *task);

#endif
