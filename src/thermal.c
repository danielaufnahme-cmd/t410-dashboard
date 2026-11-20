/*
 * thermal.c - peak temperature from sysfs and the shutdown failsafe.
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 */
#include "thermal.h"

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>

/* Reads one sensor file. sysfs reports millidegrees; the fake file may
 * contain plain degrees. Returns < 0 on failure or implausible values. */
static double read_temp(const char *path)
{
	FILE *f = fopen(path, "r");
	double v;
	if (!f)
		return -1;
	if (fscanf(f, "%lf", &v) != 1)
		v = -1;
	fclose(f);
	if (v > 1000)
		v /= 1000.0;
	if (v <= 0 || v > 150)
		return -1;
	return v;
}

static void scan(const char *pattern, double *peak)
{
	glob_t g;
	if (glob(pattern, 0, NULL, &g) != 0)
		return;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		double t = read_temp(g.gl_pathv[i]);
		if (t > *peak)
			*peak = t;
	}
	globfree(&g);
}

double thermal_read_peak(const char *fake_file)
{
	double peak = -1;
	if (fake_file)
		return read_temp(fake_file);
	scan("/sys/class/thermal/thermal_zone*/temp", &peak);
	scan("/sys/class/hwmon/hwmon*/temp*_input", &peak);
	return peak;
}

int thermal_update(thermal *t, double peak, double limit, int delay_sec, time_t now)
{
	t->peak_c = peak;
	if (peak > limit)
		t->hot = 1;
	else if (peak < limit)
		t->hot = 0;

	switch (t->state) {
	case TH_NORMAL:
		if (peak > limit) {
			t->state = TH_COUNTDOWN;
			t->deadline = now + delay_sec;
		}
		break;
	case TH_COUNTDOWN:
		if (now >= t->deadline) {
			t->state = TH_POWEROFF;
			return 1;
		}
		break;
	case TH_SNOOZED:
		if (now >= t->snooze_until) {
			if (peak > limit) {
				t->state = TH_COUNTDOWN;
				t->deadline = now + delay_sec;
			} else {
				t->state = TH_NORMAL;
			}
		}
		break;
	case TH_POWEROFF:
		break;
	}
	return 0;
}

void thermal_snooze(thermal *t, int snooze_sec, time_t now)
{
	t->state = TH_SNOOZED;
	t->snooze_until = now + snooze_sec;
	t->hot = 1;
}
