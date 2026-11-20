/*
 * weather.c - non-blocking Open-Meteo fetch via a forked wget/curl child.
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 */
#include "weather.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define REFRESH_SEC   (30 * 60)
#define RETRY_SEC     (2 * 60)   /* after a failure, e.g. network not up yet */
#define TIMEOUT_SEC   45
#define MAX_RESPONSE  (256 * 1024)

void weather_init(weather *w)
{
	memset(w, 0, sizeof(*w));
	w->fd = -1;
}

const char *wmo_text(int code)
{
	switch (code) {
	case 0: return "Clear sky";
	case 1: return "Mainly clear";
	case 2: return "Partly cloudy";
	case 3: return "Overcast";
	case 45: return "Fog";
	case 48: return "Rime fog";
	case 51: return "Light drizzle";
	case 53: return "Drizzle";
	case 55: return "Dense drizzle";
	case 56: case 57: return "Freezing drizzle";
	case 61: return "Light rain";
	case 63: return "Rain";
	case 65: return "Heavy rain";
	case 66: case 67: return "Freezing rain";
	case 71: return "Light snow";
	case 73: return "Snow";
	case 75: return "Heavy snow";
	case 77: return "Snow grains";
	case 80: return "Light showers";
	case 81: return "Showers";
	case 82: return "Heavy showers";
	case 85: case 86: return "Snow showers";
	case 95: return "Thunderstorm";
	case 96: case 99: return "Thunderstorm, hail";
	default: return "Unknown";
	}
}

static int date_wday(const char *ymd)
{
	struct tm tm;
	int y, m, d;
	if (!ymd || sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3)
		return -1;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = y - 1900;
	tm.tm_mon = m - 1;
	tm.tm_mday = d;
	tm.tm_hour = 12;
	tm.tm_isdst = -1;
	if (mktime(&tm) == (time_t)-1)
		return -1;
	return tm.tm_wday;
}

static int num(const jval *v, double *out)
{
	if (!v || v->t != J_NUM)
		return 0;
	*out = v->n;
	return 1;
}

int weather_parse(const char *json, size_t len, weather *w)
{
	jval *root = json_parse(json, len);
	jval *cur, *daily;
	double t, wind, code;
	int ok = 0;

	if (!root)
		return -1;
	cur = json_get(root, "current");
	daily = json_get(root, "daily");
	if (!num(json_get(cur, "temperature_2m"), &t) ||
	    !num(json_get(cur, "wind_speed_10m"), &wind) ||
	    !num(json_get(cur, "weather_code"), &code))
		goto out;

	w->temp_c = t;
	w->wind_kmh = wind;
	w->code = (int)code;
	w->ndays = 0;
	for (int i = 0; i < WEATHER_DAYS; i++) {
		jval *tv = json_at(json_get(daily, "time"), i);
		double lo, hi, dc;
		if (!tv || tv->t != J_STR ||
		    !num(json_at(json_get(daily, "temperature_2m_min"), i), &lo) ||
		    !num(json_at(json_get(daily, "temperature_2m_max"), i), &hi))
			break;
		if (!num(json_at(json_get(daily, "weather_code"), i), &dc))
			dc = -1;
		w->days[i].wday = date_wday(tv->s);
		w->days[i].tmin = lo;
		w->days[i].tmax = hi;
		w->days[i].code = (int)dc;
		w->ndays++;
	}
	ok = 1;
out:
	json_free(root);
	return ok ? 0 : -1;
}

static void reap(weather *w)
{
	if (w->fd >= 0) {
		close(w->fd);
		w->fd = -1;
	}
	if (w->pid > 0) {
		/* The child has closed stdout or been killed; it exits promptly. */
		while (waitpid(w->pid, NULL, 0) < 0 && errno == EINTR)
			;
		w->pid = 0;
	}
	free(w->buf);
	w->buf = NULL;
	w->len = w->cap = 0;
}

static void finish(weather *w, int success, time_t now)
{
	if (success) {
		w->valid = 1;
		w->offline = 0;
		w->updated = now;
		w->next_fetch = now + REFRESH_SEC;
	} else {
		w->offline = 1;
		w->next_fetch = now + RETRY_SEC;
	}
}

void weather_start(weather *w, double lat, double lon)
{
	char url[512];
	int p[2];
	pid_t pid;

	if (w->pid > 0)
		return;
	snprintf(url, sizeof(url),
	         "https://api.open-meteo.com/v1/forecast"
	         "?latitude=%.4f&longitude=%.4f"
	         "&current=temperature_2m,weather_code,wind_speed_10m"
	         "&daily=weather_code,temperature_2m_max,temperature_2m_min"
	         "&wind_speed_unit=kmh&timezone=auto&forecast_days=7",
	         lat, lon);

	w->started = time(NULL);
	if (pipe(p)) {
		finish(w, 0, w->started);
		return;
	}
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		finish(w, 0, w->started);
		return;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR);
		dup2(p[1], STDOUT_FILENO);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		close(p[0]);
		close(p[1]);
		signal(SIGPIPE, SIG_DFL);
		/* busybox wget on Alpine does https through ssl_client */
		execlp("wget", "wget", "-qO-", "-T", "30", url, (char *)NULL);
		execlp("curl", "curl", "-fsS", "--max-time", "30", url, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	w->pid = pid;
	w->fd = p[0];
}

void weather_cancel(weather *w)
{
	if (w->pid > 0)
		kill(w->pid, SIGKILL);
	reap(w);
}

int weather_read(weather *w)
{
	for (;;) {
		if (w->len + 4096 > w->cap) {
			if (w->cap >= MAX_RESPONSE) {
				weather_cancel(w);
				finish(w, 0, time(NULL));
				return 1;
			}
			size_t ncap = w->cap ? w->cap * 2 : 16384;
			char *nb = realloc(w->buf, ncap);
			if (!nb) {
				weather_cancel(w);
				finish(w, 0, time(NULL));
				return 1;
			}
			w->buf = nb;
			w->cap = ncap;
		}
		ssize_t r = read(w->fd, w->buf + w->len, w->cap - w->len - 1);
		if (r > 0) {
			w->len += (size_t)r;
			continue;
		}
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (r < 0 && errno == EINTR)
			continue;
		break; /* EOF or hard error */
	}

	/* EOF: collect exit status, then parse. */
	int status = 0, success = 0;
	close(w->fd);
	w->fd = -1;
	while (waitpid(w->pid, &status, 0) < 0 && errno == EINTR)
		;
	w->pid = 0;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && w->len > 0) {
		/* Parse into a copy so a bad response keeps the last good data. */
		weather tmp = *w;
		if (weather_parse(w->buf, w->len, &tmp) == 0) {
			w->temp_c = tmp.temp_c;
			w->wind_kmh = tmp.wind_kmh;
			w->code = tmp.code;
			memcpy(w->days, tmp.days, sizeof(w->days));
			w->ndays = tmp.ndays;
			success = 1;
		}
	}
	reap(w);
	finish(w, success, time(NULL));
	return 1;
}

int weather_tick(weather *w, time_t now)
{
	if (w->pid > 0 && now - w->started > TIMEOUT_SEC) {
		weather_cancel(w);
		finish(w, 0, now);
		return 1;
	}
	return 0;
}
