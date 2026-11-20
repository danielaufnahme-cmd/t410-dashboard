/*
 * config.c - config.json loading and atomic task saving.
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 */
#include "config.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char DEFAULT_CONFIG[] =
	"{\n"
	"  \"bank\": {\n"
	"    \"account_number\": \"ES00 0000 0000 0000 0000 0000\",\n"
	"    \"balance_eur\": \"0.00\",\n"
	"    \"toggle_key\": \"b\"\n"
	"  },\n"
	"  \"countdown\": {\n"
	"    \"label\": \"Target Date\",\n"
	"    \"date\": \"2027-10-24\"\n"
	"  },\n"
	"  \"weather\": {\n"
	"    \"latitude\": 36.51,\n"
	"    \"longitude\": -4.88,\n"
	"    \"city\": \"Marbella\"\n"
	"  },\n"
	"  \"thermal\": {\n"
	"    \"high_temp_c\": 85,\n"
	"    \"shutdown_delay_sec\": 10,\n"
	"    \"snooze_sec\": 60\n"
	"  },\n"
	"  \"task\": \"Complete current milestone\"\n"
	"}\n";

static int file_exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

void config_resolve_path(const char *arg, const char *argv0, char *out, size_t outlen)
{
	char exe[PATH_MAX];
	ssize_t n;
	const char *home;

	if (arg && *arg) {
		snprintf(out, outlen, "%s", arg);
		return;
	}

	/* config.json next to the binary */
	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) {
		exe[n] = '\0';
	} else {
		snprintf(exe, sizeof(exe), "%s", argv0 ? argv0 : "");
	}
	char *slash = strrchr(exe, '/');
	if (slash) {
		*slash = '\0';
		snprintf(out, outlen, "%s/config.json", exe);
		if (file_exists(out))
			return;
	}

	home = getenv("HOME");
	snprintf(out, outlen, "%s/.config/t410dash/config.json", home && *home ? home : ".");
}

static char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf = NULL;
	size_t cap = 0, n = 0;

	if (!f)
		return NULL;
	for (;;) {
		if (n + 4096 > cap) {
			if (cap > (1u << 20)) /* refuse absurdly large configs */
				goto fail;
			cap = cap ? cap * 2 : 8192;
			char *nb = realloc(buf, cap);
			if (!nb)
				goto fail;
			buf = nb;
		}
		size_t r = fread(buf + n, 1, cap - n - 1, f);
		n += r;
		if (r == 0)
			break;
	}
	if (ferror(f))
		goto fail;
	fclose(f);
	buf[n] = '\0';
	*len = n;
	return buf;
fail:
	fclose(f);
	free(buf);
	return NULL;
}

/* Field getters: user value if present and of the right type, else default. */
static void get_str(const jval *user, const jval *def, const char *path, char *out, size_t outlen)
{
	const jval *v = json_path(user, path);
	if (!v || (v->t != J_STR && v->t != J_NUM))
		v = json_path(def, path);
	snprintf(out, outlen, "%s", v && v->s ? v->s : "");
}

static double get_num(const jval *user, const jval *def, const char *path)
{
	const jval *v = json_path(user, path);
	if (v && v->t == J_STR && v->s) { /* tolerate "85" */
		char *end;
		double d = strtod(v->s, &end);
		if (end != v->s && *end == '\0')
			return d;
	}
	if (!v || v->t != J_NUM)
		v = json_path(def, path);
	return v ? v->n : 0.0;
}

int config_load(const char *path, config *cfg)
{
	jval *def = json_parse(DEFAULT_CONFIG, sizeof(DEFAULT_CONFIG) - 1);
	jval *user = NULL;
	size_t len = 0;
	char *buf = read_file(path, &len);
	int rc = -1;

	if (buf) {
		user = json_parse(buf, len);
		if (user && user->t == J_OBJ)
			rc = 0;
		else
			fprintf(stderr, "t410dash: %s: invalid JSON, using defaults\n", path);
		free(buf);
	}

	memset(cfg, 0, sizeof(*cfg));
	get_str(user, def, "bank.account_number", cfg->account_number, sizeof(cfg->account_number));
	get_str(user, def, "bank.balance_eur", cfg->balance_eur, sizeof(cfg->balance_eur));
	get_str(user, def, "bank.toggle_key", cfg->toggle_key, sizeof(cfg->toggle_key));
	get_str(user, def, "countdown.label", cfg->countdown_label, sizeof(cfg->countdown_label));
	get_str(user, def, "countdown.date", cfg->countdown_date, sizeof(cfg->countdown_date));
	cfg->latitude = get_num(user, def, "weather.latitude");
	cfg->longitude = get_num(user, def, "weather.longitude");
	get_str(user, def, "weather.city", cfg->city, sizeof(cfg->city));
	cfg->high_temp_c = get_num(user, def, "thermal.high_temp_c");
	cfg->shutdown_delay_sec = (int)get_num(user, def, "thermal.shutdown_delay_sec");
	cfg->snooze_sec = (int)get_num(user, def, "thermal.snooze_sec");
	get_str(user, def, "task", cfg->task, sizeof(cfg->task));

	if (!cfg->toggle_key[0])
		snprintf(cfg->toggle_key, sizeof(cfg->toggle_key), "b");
	if (cfg->high_temp_c <= 0)
		cfg->high_temp_c = 85;
	if (cfg->shutdown_delay_sec < 1)
		cfg->shutdown_delay_sec = 10;
	if (cfg->snooze_sec < 1)
		cfg->snooze_sec = 60;

	json_free(user);
	json_free(def);
	return rc;
}

static int mkdir_parent(const char *path)
{
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return 0;
	*slash = '\0';
	/* create each component (mkdir -p) */
	for (char *p = dir + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(dir, 0700) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(dir, 0700) && errno != EEXIST)
		return -1;
	return 0;
}

int config_save_task(const char *path, const char *task)
{
	char tmp[PATH_MAX + 16];
	size_t len = 0;
	char *buf = read_file(path, &len);
	jval *root = buf ? json_parse(buf, len) : NULL;
	mode_t mode = 0600;
	struct stat st;
	FILE *f = NULL;
	int fd = -1, rc = -1;

	int had_file = buf != NULL;

	free(buf);
	if (had_file && !root) {
		/* Never clobber a file we could not parse. */
		fprintf(stderr, "t410dash: %s: invalid JSON, task not saved\n", path);
		return -1;
	}
	if (!root)
		root = json_parse(DEFAULT_CONFIG, sizeof(DEFAULT_CONFIG) - 1);
	if (!root || root->t != J_OBJ || json_set_str(root, "task", task))
		goto out;

	if (stat(path, &st) == 0)
		mode = st.st_mode & 07777;
	else if (mkdir_parent(path))
		goto out;

	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		goto out;
	if (fchmod(fd, mode)) {
		/* not fatal: keep 0600 */
	}
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		goto out;
	}
	if (json_write(f, root) || fflush(f) || fsync(fileno(f))) {
		fclose(f);
		unlink(tmp);
		goto out;
	}
	if (fclose(f)) {
		unlink(tmp);
		goto out;
	}
	if (rename(tmp, path)) {
		unlink(tmp);
		goto out;
	}
	rc = 0;
out:
	if (rc)
		fprintf(stderr, "t410dash: saving %s failed: %s\n", path, strerror(errno));
	json_free(root);
	return rc;
}
