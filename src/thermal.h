/*
 * thermal.h - peak temperature from sysfs and the shutdown failsafe.
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 */
#ifndef T410DASH_THERMAL_H
#define T410DASH_THERMAL_H

#include <time.h>

typedef enum {
	TH_NORMAL,     /* below threshold */
	TH_COUNTDOWN,  /* warning overlay, counting down to poweroff */
	TH_SNOOZED,    /* user cancelled; re-arms after snooze_sec */
	TH_POWEROFF    /* poweroff issued */
} thermal_state;

typedef struct {
	double peak_c;       /* latest peak reading, < 0 if none */
	thermal_state state;
	int hot;             /* high-temp indicator (stays until temp < limit) */
	time_t deadline;     /* countdown end */
	time_t snooze_until;
} thermal;

/* Peak temperature in degrees C over all thermal zones and hwmon sensors,
 * or from fake_file if given. Returns < 0 if nothing could be read. */
double thermal_read_peak(const char *fake_file);

/* Advance the state machine. Returns 1 if poweroff should run now. */
int thermal_update(thermal *t, double peak, double limit, int delay_sec, time_t now);

/* A key was pressed during the countdown. */
void thermal_snooze(thermal *t, int snooze_sec, time_t now);

#endif
