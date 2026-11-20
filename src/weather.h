/*
 * weather.h - non-blocking Open-Meteo fetch via a forked wget/curl child.
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 */
#ifndef T410DASH_WEATHER_H
#define T410DASH_WEATHER_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define WEATHER_DAYS 7

typedef struct {
	int wday;          /* 0 = Sunday */
	int code;          /* WMO weather code */
	double tmin, tmax;
} weather_day;

typedef struct {
	/* last good data */
	int valid;
	double temp_c, wind_kmh;
	int code;
	weather_day days[WEATHER_DAYS];
	int ndays;
	time_t updated;    /* time of last successful update */

	/* fetch state */
	int offline;       /* last attempt failed */
	pid_t pid;         /* running child, 0 if none */
	int fd;            /* read end of the child's stdout, -1 if none */
	char *buf;
	size_t len, cap;
	time_t started;
	time_t next_fetch;
} weather;

void weather_init(weather *w);
/* Start a fetch unless one is already running. */
void weather_start(weather *w, double lat, double lon);
/* Kill a running fetch (e.g. before a forced refresh). */
void weather_cancel(weather *w);
/* Call when w->fd is readable. Returns 1 when the fetch finished
 * (successfully or not) and the display should be updated. */
int weather_read(weather *w);
/* Call periodically: enforces the fetch timeout. Returns 1 on change. */
int weather_tick(weather *w, time_t now);

const char *wmo_text(int code);

/* Exposed for tests: parse an Open-Meteo forecast response. */
int weather_parse(const char *json, size_t len, weather *w);

#endif
