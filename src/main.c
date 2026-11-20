/*
 * t410dash - ultra-low-RAM fullscreen kiosk dashboard (Xlib + Xft).
 *
 * Copyright (c) 2026 Daniel. MIT License, see LICENSE.
 *
 * Single-threaded: one poll() loop over the X connection and the weather
 * child's pipe, with a once-per-second tick for the clock, thermal checks
 * and timers. Everything is drawn into a back-buffer pixmap and copied to
 * the window in one go, so there is no flicker.
 */
#include "config.h"
#include "thermal.h"
#include "weather.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1.0.0"
#define CURSOR_IDLE_MS 3000
#define STATUS_SEC 4

#define FONT_SANS "DejaVu Sans"
#define FONT_MONO "DejaVu Sans Mono"

enum {
	C_BG, C_SURFACE, C_BORDER, C_TEXT, C_TEXT2, C_MUTED, C_ACCENT,
	C_TRACK, C_AMBER, C_RED, C_RED_DIM, C_BLACK, C_WHITE, NCOLORS
};

static const unsigned palette[NCOLORS] = {
	[C_BG]      = 0x0B0D0F,
	[C_SURFACE] = 0x121518,
	[C_BORDER]  = 0x1F2429,
	[C_TEXT]    = 0xE8EBED,
	[C_TEXT2]   = 0xA2ABB3,
	[C_MUTED]   = 0x646E78,
	[C_ACCENT]  = 0x76CCB1,
	[C_TRACK]   = 0x22282D,
	[C_AMBER]   = 0xFFB02E,
	[C_RED]     = 0xFF453A,
	[C_RED_DIM] = 0x3A110E,
	[C_BLACK]   = 0x000000,
	[C_WHITE]   = 0xFFFFFF,
};

typedef struct {
	XftFont *xft;
	int px;
} font;

enum {
	F_LABEL, F_SMALL, F_BODY, F_MED, F_MONO_BODY, F_NUM, F_BIG,
	F_CLOCK, F_WARN_TITLE, F_WARN_HUGE, NFONTS
};

static struct {
	Display *dpy;
	int scr;
	Window win;
	Visual *vis;
	Colormap cmap;
	Pixmap buf;
	XftDraw *draw;
	GC gc;
	int w, h;
	XIM xim;
	XIC xic;
	Cursor blank;
	int cursor_hidden;
	long long last_motion_ms;
	Atom wm_delete;

	XftColor col[NCOLORS];
	font f[NFONTS];

	config cfg;
	char cfg_path[PATH_MAX];

	int bank_revealed;
	int editing;
	char edit[TASK_MAX];
	size_t edit_len;

	weather wx;
	thermal th;
	time_t next_thermal;
	pid_t poweroff_pid;

	const char *fake_temp;
	int dry_run;

	char status[128];
	time_t status_until;

	int dirty;
} A;

static volatile sig_atomic_t running = 1;

static const char *WDAY_SHORT[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *WDAY_LONG[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                   "Thursday", "Friday", "Saturday" };
static const char *MONTH_LONG[] = { "January", "February", "March", "April", "May", "June",
                                    "July", "August", "September", "October", "November",
                                    "December" };
static const char *MONTH_SHORT[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ------------------------------------------------------------------ */
/* utilities                                                           */

static long long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static void set_status(const char *msg)
{
	snprintf(A.status, sizeof(A.status), "%s", msg);
	A.status_until = time(NULL) + STATUS_SEC;
	A.dirty = 1;
}

static void hhmm(time_t t, char *out, size_t len)
{
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, len, "%H:%M", &tm);
}

/* "1234567.80" -> "1,234,567.80"; anything non-numeric is returned as is. */
static void group_thousands(const char *in, char *out, size_t len)
{
	const char *p = in, *dot;
	size_t intlen, o = 0;

	if (*p == '-')
		p++;
	dot = strchr(p, '.');
	intlen = dot ? (size_t)(dot - p) : strlen(p);
	for (const char *q = p; *q; q++)
		if (!isdigit((unsigned char)*q) && q != dot) {
			snprintf(out, len, "%s", in);
			return;
		}
	if (intlen == 0) {
		snprintf(out, len, "%s", in);
		return;
	}
	if (p != in && o + 1 < len)
		out[o++] = '-';
	for (size_t i = 0; i < intlen && o + 2 < len; i++) {
		if (i > 0 && (intlen - i) % 3 == 0)
			out[o++] = ',';
		out[o++] = p[i];
	}
	if (dot)
		for (const char *q = dot; *q && o + 1 < len; q++)
			out[o++] = *q;
	out[o] = '\0';
}

/* Whole days from today to the YYYY-MM-DD date (local time). */
static int days_until(const char *ymd, time_t now, struct tm *target_out)
{
	struct tm t, today;
	int y, m, d;

	if (sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3)
		return INT_MIN;
	memset(&t, 0, sizeof(t));
	t.tm_year = y - 1900;
	t.tm_mon = m - 1;
	t.tm_mday = d;
	t.tm_hour = 12;
	t.tm_isdst = -1;
	time_t target = mktime(&t);
	if (target == (time_t)-1)
		return INT_MIN;
	if (target_out)
		*target_out = t;
	localtime_r(&now, &today);
	today.tm_hour = 12;
	today.tm_min = today.tm_sec = 0;
	today.tm_isdst = -1;
	return (int)lround(difftime(target, mktime(&today)) / 86400.0);
}

/* ------------------------------------------------------------------ */
/* drawing primitives                                                  */

static int px(double v)
{
	return (int)lround(v);
}

static void fill(int x, int y, int w, int h, int c)
{
	if (w > 0 && h > 0)
		XftDrawRect(A.draw, &A.col[c], x, y, (unsigned)w, (unsigned)h);
}

static void panel(int x, int y, int w, int h)
{
	fill(x, y, w, h, C_BORDER);
	fill(x + 1, y + 1, w - 2, h - 2, C_SURFACE);
}

static int text_w(int f, const char *s)
{
	XGlyphInfo g;
	XftTextExtentsUtf8(A.dpy, A.f[f].xft, (const FcChar8 *)s, (int)strlen(s), &g);
	return g.xOff;
}

/* Cap height: top of digits/capitals above the baseline (DejaVu ~0.73 em). */
static int cap(int f)
{
	return px(A.f[f].px * 0.73);
}

static void text(int f, int c, int x, int y, const char *s)
{
	XftDrawStringUtf8(A.draw, &A.col[c], A.f[f].xft, x, y, (const FcChar8 *)s, (int)strlen(s));
}

static void text_right(int f, int c, int right, int y, const char *s)
{
	text(f, c, right - text_w(f, s), y, s);
}

static void text_center(int f, int c, int cx, int y, const char *s)
{
	text(f, c, cx - text_w(f, s) / 2, y, s);
}

static size_t utf8_len(unsigned char c)
{
	if (c < 0x80) return 1;
	if ((c & 0xE0) == 0xC0) return 2;
	if ((c & 0xF0) == 0xE0) return 3;
	if ((c & 0xF8) == 0xF0) return 4;
	return 1;
}

/* Uppercase label with letter spacing. Returns the width drawn. */
static int label_w(const char *s, int spacing)
{
	int w = 0;
	char ch[8];
	for (const char *p = s; *p;) {
		size_t n = utf8_len((unsigned char)*p);
		memcpy(ch, p, n);
		ch[n] = '\0';
		if (n == 1)
			ch[0] = (char)toupper((unsigned char)ch[0]);
		w += text_w(F_LABEL, ch) + spacing;
		p += n;
	}
	return w > 0 ? w - spacing : 0;
}

static int label(int c, int x, int y, const char *s)
{
	int spacing = px(A.f[F_LABEL].px * 0.12);
	char ch[8];
	int x0 = x;
	for (const char *p = s; *p;) {
		size_t n = utf8_len((unsigned char)*p);
		memcpy(ch, p, n);
		ch[n] = '\0';
		if (n == 1)
			ch[0] = (char)toupper((unsigned char)ch[0]);
		text(F_LABEL, c, x, y, ch);
		x += text_w(F_LABEL, ch) + spacing;
		p += n;
	}
	return x - x0;
}

static void label_right(int c, int right, int y, const char *s)
{
	int spacing = px(A.f[F_LABEL].px * 0.12);
	label(c, right - label_w(s, spacing), y, s);
}

/* Copy s into out, truncated with an ellipsis so it fits in maxw. */
static void fit(int f, const char *s, int maxw, char *out, size_t len)
{
	size_t n;
	snprintf(out, len, "%s", s);
	if (text_w(f, out) <= maxw)
		return;
	n = strlen(out);
	while (n > 0) {
		do
			n--;
		while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80);
		if (n + 4 > len)
			continue;
		memcpy(out + n, "\xE2\x80\xA6", 4); /* … */
		if (text_w(f, out) <= maxw)
			return;
	}
	out[0] = '\0';
}

/* Section label with an optional muted suffix ("WEATHER · MARBELLA"). */
static void section(int x, int y, const char *title, const char *suffix)
{
	int w = label(C_MUTED, x, y, title);
	if (suffix && *suffix) {
		char buf[96];
		snprintf(buf, sizeof(buf), "  \xC2\xB7  %s", suffix);
		label(C_MUTED, x + w, y, buf);
	}
}

/* ------------------------------------------------------------------ */
/* fonts and colours                                                   */

static XftFont *open_font(const char *family, int weight, int pixels)
{
	XftFont *f = XftFontOpen(A.dpy, A.scr,
	                         XFT_FAMILY, XftTypeString, family,
	                         XFT_PIXEL_SIZE, XftTypeDouble, (double)pixels,
	                         XFT_WEIGHT, XftTypeInteger, weight,
	                         XFT_ANTIALIAS, XftTypeBool, True,
	                         NULL);
	if (!f)
		f = XftFontOpenName(A.dpy, A.scr, "sans");
	return f;
}

static void close_fonts(void)
{
	for (int i = 0; i < NFONTS; i++)
		if (A.f[i].xft) {
			XftFontClose(A.dpy, A.f[i].xft);
			A.f[i].xft = NULL;
		}
}

static void load_fonts(void)
{
	/* Scale from the 1280x800 design; use the tighter axis. */
	double u = fmin(A.h / 800.0, A.w / 1280.0);
	struct { const char *fam; int weight; double size; } spec[NFONTS] = {
		[F_LABEL]      = { FONT_SANS, FC_WEIGHT_BOLD,    11.5 },
		[F_SMALL]      = { FONT_SANS, FC_WEIGHT_REGULAR, 13 },
		[F_BODY]       = { FONT_SANS, FC_WEIGHT_REGULAR, 17 },
		[F_MED]        = { FONT_SANS, FC_WEIGHT_REGULAR, 25 },
		[F_MONO_BODY]  = { FONT_MONO, FC_WEIGHT_REGULAR, 17 },
		[F_NUM]   = { FONT_SANS, FC_WEIGHT_REGULAR, 30 },
		[F_BIG]        = { FONT_SANS, FC_WEIGHT_REGULAR, 66 },
		[F_CLOCK]      = { FONT_SANS, FC_WEIGHT_REGULAR, 150 },
		[F_WARN_TITLE] = { FONT_SANS, FC_WEIGHT_BOLD,    40 },
		[F_WARN_HUGE]  = { FONT_SANS, FC_WEIGHT_BOLD,    300 },
	};
	close_fonts();
	for (int i = 0; i < NFONTS; i++) {
		A.f[i].px = px(spec[i].size * u);
		A.f[i].xft = open_font(spec[i].fam, spec[i].weight, A.f[i].px);
		if (!A.f[i].xft) {
			fprintf(stderr, "t410dash: cannot open any font\n");
			exit(1);
		}
	}
}

static void alloc_colors(void)
{
	for (int i = 0; i < NCOLORS; i++) {
		XRenderColor rc = {
			(unsigned short)(((palette[i] >> 16) & 0xFF) * 257),
			(unsigned short)(((palette[i] >> 8) & 0xFF) * 257),
			(unsigned short)((palette[i] & 0xFF) * 257),
			0xFFFF,
		};
		XftColorAllocValue(A.dpy, A.vis, A.cmap, &rc, &A.col[i]);
	}
}

/* ------------------------------------------------------------------ */
/* dashboard                                                           */

static void draw_header(int x, int y, int w, int h, const struct tm *now)
{
	char buf[64];
	int fc = F_CLOCK;
	int clock_cap = cap(fc);
	int date_gap = px(A.f[F_MED].px * 1.85);
	int block = clock_cap + date_gap;
	int base = y + (h - block) / 2 + clock_cap;

	(void)w;
	snprintf(buf, sizeof(buf), "%02d:%02d", now->tm_hour, now->tm_min);
	text(fc, C_TEXT, x - px(A.f[fc].px * 0.04), base, buf);
	int cx = x - px(A.f[fc].px * 0.04) + text_w(fc, buf);
	snprintf(buf, sizeof(buf), ":%02d", now->tm_sec);
	text(fc, C_MUTED, cx, base, buf);

	snprintf(buf, sizeof(buf), "%s, %d %s %d", WDAY_LONG[now->tm_wday], now->tm_mday,
	         MONTH_LONG[now->tm_mon], now->tm_year + 1900);
	text(F_MED, C_TEXT2, x, base + date_gap, buf);
}

static void draw_countdown(int x, int y, int w, int h, time_t t)
{
	char buf[96], fitted[96];
	int pad = px(A.f[F_BODY].px * 1.3);
	struct tm target = { 0 };
	int days = days_until(A.cfg.countdown_date, t, &target);

	panel(x, y, w, h);
	fit(F_LABEL, A.cfg.countdown_label, w - 2 * pad, fitted, sizeof(fitted));
	section(x + pad, y + pad + cap(F_LABEL), fitted, NULL);

	int base = y + h / 2 + cap(F_BIG) / 2 + px(A.f[F_LABEL].px * 0.6);
	if (days == INT_MIN) {
		text(F_MED, C_AMBER, x + pad, base, "Invalid date");
		snprintf(buf, sizeof(buf), "countdown.date = \"%s\"", A.cfg.countdown_date);
		fit(F_SMALL, buf, w - 2 * pad, fitted, sizeof(fitted));
		text(F_SMALL, C_MUTED, x + pad, y + h - pad, fitted);
		return;
	}
	snprintf(buf, sizeof(buf), "%d", days < 0 ? 0 : days);
	text(F_BIG, C_ACCENT, x + pad - px(A.f[F_BIG].px * 0.03), base, buf);
	int nx = x + pad + text_w(F_BIG, buf) + px(A.f[F_BODY].px * 0.6);
	text(F_BODY, C_TEXT2, nx, base, days == 1 ? "day left" : "days left");

	if (days <= 0)
		snprintf(buf, sizeof(buf), "Reached \xC2\xB7 %s, %d %s %d", WDAY_SHORT[target.tm_wday],
		         target.tm_mday, MONTH_SHORT[target.tm_mon], target.tm_year + 1900);
	else
		snprintf(buf, sizeof(buf), "Until %s, %d %s %d", WDAY_SHORT[target.tm_wday],
		         target.tm_mday, MONTH_SHORT[target.tm_mon], target.tm_year + 1900);
	text(F_SMALL, C_MUTED, x + pad, y + h - pad, buf);
}

static void draw_weather(int x, int y, int w, int h)
{
	char buf[96], fitted[96];
	int pad = px(A.f[F_BODY].px * 1.3);
	int ix = x + pad, iw = w - 2 * pad;
	int label_base = y + pad + cap(F_LABEL);
	weather *wx = &A.wx;

	panel(x, y, w, h);
	section(ix, label_base, "Weather", A.cfg.city);

	if (!wx->valid) {
		const char *msg = wx->offline ? "Offline \xC2\xB7 no data yet" : "Loading weather\xE2\x80\xA6";
		text(F_MED, wx->offline ? C_AMBER : C_TEXT2, ix, y + h / 2 + cap(F_MED) / 2, msg);
		return;
	}

	/* Current conditions, left column, centred vertically below the label. */
	int lw = px(iw * 0.40);
	int line = px(A.f[F_BODY].px * 1.8);
	int cond_gap = px(A.f[F_MED].px * 1.9);
	int block = cap(F_BIG) + cond_gap + px(A.f[F_BODY].px * 0.6) + 2 * line;
	int area_top = label_base + px(A.f[F_LABEL].px * 1.2);
	int area_h = y + h - pad - area_top;
	int tb = area_top + (area_h - block) / 2 + cap(F_BIG);
	snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lround(wx->temp_c));
	text(F_BIG, C_TEXT, ix - px(A.f[F_BIG].px * 0.03), tb, buf);

	int cb = tb + cond_gap;
	fit(F_MED, wmo_text(wx->code), lw - pad / 2, fitted, sizeof(fitted));
	text(F_MED, C_TEXT, ix, cb, fitted);

	int lb = cb + px(A.f[F_BODY].px * 0.6) + line;
	snprintf(buf, sizeof(buf), "Wind  %d km/h", (int)lround(wx->wind_kmh));
	text(F_BODY, C_TEXT2, ix, lb, buf);
	if (wx->ndays > 0) {
		snprintf(buf, sizeof(buf), "High  %d\xC2\xB0   Low  %d\xC2\xB0",
		         (int)lround(wx->days[0].tmax), (int)lround(wx->days[0].tmin));
		text(F_BODY, C_TEXT2, ix, lb + line, buf);
	}

	/* 7-day forecast, right column. */
	int fx = ix + lw + pad;
	int fw = ix + iw - fx;
	int ftop = y + pad / 2;
	int fbot = y + h - pad / 2;
	int n = wx->ndays;
	if (n <= 0)
		return;

	fill(fx - pad, y + pad, 1, h - 2 * pad, C_BORDER);

	double wmin = wx->days[0].tmin, wmax = wx->days[0].tmax;
	for (int i = 1; i < n; i++) {
		wmin = fmin(wmin, wx->days[i].tmin);
		wmax = fmax(wmax, wx->days[i].tmax);
	}
	double range = wmax - wmin > 1 ? wmax - wmin : 1;

	int rowh = (fbot - ftop) / n;
	int day_w = px(fw * 0.19);
	int cond_w = px(fw * 0.34);
	int num_w = text_w(F_BODY, "-00\xC2\xB0");
	int bar_x = fx + day_w + cond_w + num_w + px(A.f[F_BODY].px * 0.7);
	int bar_w = fx + fw - num_w - px(A.f[F_BODY].px * 0.7) - bar_x;
	int bar_h = px(fmax(3, A.f[F_BODY].px * 0.24));

	for (int i = 0; i < n; i++) {
		weather_day *d = &wx->days[i];
		int ry = ftop + i * rowh;
		int base = ry + rowh / 2 + cap(F_BODY) / 2;
		if (i > 0)
			fill(fx, ry, fw, 1, C_BORDER);

		const char *dn = i == 0 ? "Today" : d->wday >= 0 ? WDAY_SHORT[d->wday] : "?";
		text(F_BODY, i == 0 ? C_TEXT : C_TEXT2, fx, base, dn);

		fit(F_SMALL, d->code >= 0 ? wmo_text(d->code) : "", cond_w - pad / 2, fitted, sizeof(fitted));
		text(F_SMALL, C_MUTED, fx + day_w, base, fitted);

		snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lround(d->tmin));
		text_right(F_BODY, C_MUTED, bar_x - px(A.f[F_BODY].px * 0.7), base, buf);
		snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lround(d->tmax));
		text_right(F_BODY, C_TEXT, fx + fw, base, buf);

		if (bar_w > 10) {
			int by = base - cap(F_BODY) / 2 - bar_h / 2;
			int s = px(bar_w * (d->tmin - wmin) / range);
			int e = px(bar_w * (d->tmax - wmin) / range);
			fill(bar_x, by, bar_w, bar_h, C_TRACK);
			fill(bar_x + s, by, e - s > bar_h ? e - s : bar_h, bar_h, C_ACCENT);
		}
	}
}

static void draw_bank(int x, int y, int w, int h)
{
	char buf[96], fitted[96];
	int pad = px(A.f[F_BODY].px * 1.3);
	int label_base = y + pad + cap(F_LABEL);
	char hint[32];

	panel(x, y, w, h);
	section(x + pad, label_base, "Account", NULL);
	snprintf(hint, sizeof(hint), "%s  %s", A.cfg.toggle_key, A.bank_revealed ? "hide" : "reveal");
	label_right(A.bank_revealed ? C_ACCENT : C_MUTED, x + w - pad, label_base, hint);

	int bal_base = y + h - pad;
	int acc_base = bal_base - cap(F_NUM) - px(A.f[F_MONO_BODY].px * 1.6);

	if (A.bank_revealed) {
		fit(F_MONO_BODY, A.cfg.account_number, w - 2 * pad, fitted, sizeof(fitted));
		text(F_MONO_BODY, C_TEXT2, x + pad, acc_base, fitted);
		char grouped[48];
		group_thousands(A.cfg.balance_eur, grouped, sizeof(grouped));
		snprintf(buf, sizeof(buf), "\xE2\x82\xAC %s", grouped);
	} else {
		text(F_MONO_BODY, C_TEXT2, x + pad, acc_base, "**** **** ****");
		snprintf(buf, sizeof(buf), "\xE2\x82\xAC \xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
	}
	fit(F_NUM, buf, w - 2 * pad, fitted, sizeof(fitted));
	text(F_NUM, C_TEXT, x + pad, bal_base, fitted);
}

static void draw_system(int x, int y, int w, int h)
{
	char buf[64];
	int pad = px(A.f[F_BODY].px * 1.3);
	int label_base = y + pad + cap(F_LABEL);
	double peak = A.th.peak_c, limit = A.cfg.high_temp_c;
	int hot = A.th.hot;

	panel(x, y, w, h);
	section(x + pad, label_base, "System", NULL);
	label_right(hot ? C_AMBER : peak < 0 ? C_MUTED : C_ACCENT, x + w - pad, label_base,
	            hot ? "High temp" : peak < 0 ? "No sensor" : "Normal");

	int bar_h = px(fmax(3, A.f[F_BODY].px * 0.24));
	int bar_y = y + h - pad - bar_h;
	int vb = bar_y - px(A.f[F_BODY].px * 0.9);

	if (peak < 0)
		snprintf(buf, sizeof(buf), "\xE2\x80\x94 \xC2\xB0" "C");
	else
		snprintf(buf, sizeof(buf), "%d \xC2\xB0" "C", (int)lround(peak));
	text(F_NUM, hot ? C_AMBER : C_TEXT, x + pad, vb, buf);
	snprintf(buf, sizeof(buf), "peak \xC2\xB7 limit %d \xC2\xB0" "C", (int)lround(limit));
	text_right(F_SMALL, C_MUTED, x + w - pad, vb, buf);

	int bw = w - 2 * pad;
	fill(x + pad, bar_y, bw, bar_h, C_TRACK);
	if (peak > 0) {
		double frac = fmin(1.0, peak / (limit + 15.0));
		fill(x + pad, bar_y, px(bw * frac), bar_h, hot ? C_AMBER : C_ACCENT);
		int lx = x + pad + px(bw * fmin(1.0, limit / (limit + 15.0)));
		fill(lx, bar_y - bar_h, 1, bar_h * 3, C_MUTED);
	}
}

static void draw_task(int x, int y, int w, int h)
{
	char fitted[TASK_MAX + 8];
	int pad = px(A.f[F_BODY].px * 1.3);
	int label_base = y + pad + cap(F_LABEL);

	panel(x, y, w, h);
	section(x + pad, label_base, "Focus", A.editing ? "editing" : NULL);

	if (!A.editing) {
		label_right(C_MUTED, x + w - pad, label_base, "e  edit");
		int base = y + h - pad - px(A.f[F_MED].px * 0.12);
		const char *t = A.cfg.task[0] ? A.cfg.task : "No task set";
		fit(F_MED, t, w - 2 * pad, fitted, sizeof(fitted));
		text(F_MED, A.cfg.task[0] ? C_TEXT : C_MUTED, x + pad, base, fitted);
		return;
	}

	label_right(C_MUTED, x + w - pad, label_base, "Enter  save    Esc  cancel");

	/* Input field. */
	int fh = px(A.f[F_MED].px * 1.7);
	int fy = y + h - pad / 2 - fh - px(A.f[F_MED].px * 0.1);
	int fx = x + pad / 2, fwid = w - pad;
	fill(fx, fy, fwid, fh, C_ACCENT);
	fill(fx + 1, fy + 1, fwid - 2, fh - 2, C_BG);

	int tx = fx + pad / 2;
	int avail = fwid - pad - 4;
	int tw = text_w(F_MED, A.edit);
	int off = tw > avail ? tw - avail : 0;
	int base = fy + fh / 2 + cap(F_MED) / 2;

	XRectangle clip = { (short)(fx + 2), (short)(fy + 1), (unsigned short)(fwid - 4),
	                    (unsigned short)(fh - 2) };
	XftDrawSetClipRectangles(A.draw, 0, 0, &clip, 1);
	text(F_MED, C_TEXT, tx - off, base, A.edit);
	XftDrawSetClip(A.draw, NULL);

	int caret_h = px(A.f[F_MED].px * 1.05);
	fill(tx - off + tw + 2, base - cap(F_MED) - px(A.f[F_MED].px * 0.12), 2, caret_h, C_ACCENT);
}

static int keycap(int x, int base, const char *key, const char *desc)
{
	int kh = px(A.f[F_SMALL].px * 1.55);
	int kw = text_w(F_LABEL, key) + px(A.f[F_SMALL].px * 0.9);
	if (kw < kh)
		kw = kh;
	int ky = base - cap(F_SMALL) / 2 - kh / 2;
	fill(x, ky, kw, kh, C_BORDER);
	fill(x + 1, ky + 1, kw - 2, kh - 2, C_BG);
	text_center(F_LABEL, C_TEXT2, x + kw / 2, ky + kh / 2 + cap(F_LABEL) / 2, key);
	x += kw + px(A.f[F_SMALL].px * 0.5);
	text(F_SMALL, C_MUTED, x, base, desc);
	return x + text_w(F_SMALL, desc) + px(A.f[F_SMALL].px * 1.8);
}

static void draw_footer(int x, int y, int w, int h, time_t t)
{
	char buf[128], tk[8];
	int base = y + h / 2 + cap(F_SMALL) / 2;
	int right = x + w;

	if (A.editing) {
		x = keycap(x, base, "Enter", "Save task");
		x = keycap(x, base, "Esc", "Cancel");
		keycap(x, base, "\xE2\x8C\xAB", "Delete");
	} else {
		snprintf(tk, sizeof(tk), "%s", A.cfg.toggle_key);
		tk[0] = (char)toupper((unsigned char)tk[0]);
		x = keycap(x, base, tk, A.bank_revealed ? "Hide account" : "Show account");
		x = keycap(x, base, "E", "Edit task");
		x = keycap(x, base, "R", "Reload");
		keycap(x, base, "Q", "Quit");
	}

	if (A.status[0] && t < A.status_until) {
		text_right(F_SMALL, C_ACCENT, right, base, A.status);
		return;
	}
	char tm[16];
	if (A.wx.offline) {
		if (A.wx.valid) {
			hhmm(A.wx.updated, tm, sizeof(tm));
			snprintf(buf, sizeof(buf), "Offline \xC2\xB7 last update %s", tm);
		} else {
			snprintf(buf, sizeof(buf), "Offline \xC2\xB7 no weather data");
		}
		text_right(F_SMALL, C_AMBER, right, base, buf);
	} else if (A.wx.valid) {
		hhmm(A.wx.updated, tm, sizeof(tm));
		snprintf(buf, sizeof(buf), "Open-Meteo \xC2\xB7 updated %s", tm);
		text_right(F_SMALL, C_MUTED, right, base, buf);
	} else {
		text_right(F_SMALL, C_MUTED, right, base, "Fetching weather\xE2\x80\xA6");
	}
}

static void draw_hot_banner(int h, time_t t)
{
	char buf[128];
	fill(0, 0, A.w, h, C_AMBER);
	if (A.th.state == TH_SNOOZED && A.th.snooze_until > t)
		snprintf(buf, sizeof(buf),
		         "HIGH TEMPERATURE  \xC2\xB7  %d \xC2\xB0" "C  \xC2\xB7  shutdown snoozed, re-arms in %ld s",
		         (int)lround(A.th.peak_c), (long)(A.th.snooze_until - t));
	else
		snprintf(buf, sizeof(buf), "HIGH TEMPERATURE  \xC2\xB7  %d \xC2\xB0" "C",
		         (int)lround(A.th.peak_c));
	text_center(F_LABEL, C_BLACK, A.w / 2, h / 2 + cap(F_LABEL) / 2, buf);
}

static void draw_warning(time_t t)
{
	char buf[96];
	int W = A.w, H = A.h;
	int border = px(H * 0.008);

	fill(0, 0, W, H, C_BLACK);
	fill(0, 0, W, border, C_RED);
	fill(0, H - border, W, border, C_RED);
	fill(0, 0, border, H, C_RED);
	fill(W - border, 0, border, H, C_RED);

	if (A.th.state == TH_POWEROFF) {
		text_center(F_WARN_TITLE, C_RED, W / 2, H / 2, "Powering off\xE2\x80\xA6");
		return;
	}

	int spacing = px(A.f[F_LABEL].px * 0.12);
	label(C_AMBER, W / 2 - label_w("Thermal warning", spacing) / 2, px(H * 0.15), "Thermal warning");

	snprintf(buf, sizeof(buf), "Temperature %d \xC2\xB0" "C", (int)lround(A.th.peak_c));
	text_center(F_WARN_TITLE, C_RED, W / 2, px(H * 0.15) + px(A.f[F_WARN_TITLE].px * 1.6), buf);
	snprintf(buf, sizeof(buf), "Limit %d \xC2\xB0" "C  \xC2\xB7  the system will power off in",
	         (int)lround(A.cfg.high_temp_c));
	text_center(F_MED, C_WHITE, W / 2, px(H * 0.15) + px(A.f[F_WARN_TITLE].px * 2.9), buf);

	long left = (long)(A.th.deadline - t);
	if (left < 0)
		left = 0;
	snprintf(buf, sizeof(buf), "%ld", left);
	int huge_base = px(H * 0.43) + cap(F_WARN_HUGE) / 2 + px(H * 0.07);
	text_center(F_WARN_HUGE, C_RED, W / 2, huge_base, buf);

	int bw = px(W * 0.5), bh = px(fmax(4, H * 0.008));
	int bx = (W - bw) / 2, by = huge_base + px(H * 0.06);
	double frac = (double)left / (A.cfg.shutdown_delay_sec > 0 ? A.cfg.shutdown_delay_sec : 1);
	fill(bx, by, bw, bh, C_RED_DIM);
	fill(bx, by, px(bw * fmin(1.0, frac)), bh, C_RED);

	text_center(F_MED, C_AMBER, W / 2, by + px(H * 0.09), "Press any key to cancel");
	if (A.dry_run)
		text_center(F_SMALL, C_MUTED, W / 2, H - px(H * 0.05), "dry run \xC2\xB7 poweroff will only be printed");
}

static void render(void)
{
	time_t t = time(NULL);
	struct tm now;
	int W = A.w, H = A.h;

	localtime_r(&t, &now);

	if (A.th.state == TH_COUNTDOWN || A.th.state == TH_POWEROFF) {
		draw_warning(t);
		goto present;
	}

	fill(0, 0, W, H, C_BG);

	int M = px(W * 0.045);
	int gap = px(H * 0.022);
	int top = px(H * 0.075);
	int footer_h = px(H * 0.075);
	int bottom = H - footer_h;
	int avail = bottom - top;
	int hdr_h = px(avail * 0.30);
	int task_h = px(avail * 0.165);
	int mid_h = avail - hdr_h - task_h - 2 * gap;
	int cw = W - 2 * M;
	int rc_w = px((cw - gap) * 0.34);
	int lc_w = cw - gap - rc_w;
	int xr = M + lc_w + gap;

	if (A.th.hot)
		draw_hot_banner(px(H * 0.045), t);

	draw_header(M, top, lc_w, hdr_h, &now);
	draw_countdown(xr, top, rc_w, hdr_h, t);

	int my = top + hdr_h + gap;
	draw_weather(M, my, lc_w, mid_h);
	int bank_h = px((mid_h - gap) * 0.55);
	draw_bank(xr, my, rc_w, bank_h);
	draw_system(xr, my + bank_h + gap, rc_w, mid_h - bank_h - gap);

	draw_task(M, my + mid_h + gap, cw, task_h);
	draw_footer(M, bottom, cw, footer_h - px(H * 0.01), t);

present:
	XCopyArea(A.dpy, A.buf, A.win, A.gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
}

/* ------------------------------------------------------------------ */
/* window setup                                                        */

static void resize_buffer(int w, int h)
{
	if (A.buf && w == A.w && h == A.h)
		return;
	A.w = w;
	A.h = h;
	if (A.buf)
		XFreePixmap(A.dpy, A.buf);
	A.buf = XCreatePixmap(A.dpy, A.win, (unsigned)w, (unsigned)h,
	                      (unsigned)DefaultDepth(A.dpy, A.scr));
	if (A.draw)
		XftDrawChange(A.draw, A.buf);
	else
		A.draw = XftDrawCreate(A.dpy, A.buf, A.vis, A.cmap);
	load_fonts();
	A.dirty = 1;
}

static void create_window(void)
{
	XSetWindowAttributes wa;
	XClassHint ch = { "t410dash", "t410dash" };
	int w, h;

	A.scr = DefaultScreen(A.dpy);
	A.vis = DefaultVisual(A.dpy, A.scr);
	A.cmap = DefaultColormap(A.dpy, A.scr);
	w = DisplayWidth(A.dpy, A.scr);
	h = DisplayHeight(A.dpy, A.scr);

	wa.background_pixel = BlackPixel(A.dpy, A.scr);
	wa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask | PointerMotionMask;
	wa.bit_gravity = NorthWestGravity;
	A.win = XCreateWindow(A.dpy, RootWindow(A.dpy, A.scr), 0, 0, (unsigned)w, (unsigned)h, 0,
	                      CopyFromParent, InputOutput, CopyFromParent,
	                      CWBackPixel | CWEventMask | CWBitGravity, &wa);

	XSetClassHint(A.dpy, A.win, &ch);
	XStoreName(A.dpy, A.win, "t410dash");

	Atom state = XInternAtom(A.dpy, "_NET_WM_STATE", False);
	Atom fs = XInternAtom(A.dpy, "_NET_WM_STATE_FULLSCREEN", False);
	XChangeProperty(A.dpy, A.win, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&fs, 1);

	/* No decorations, for window managers that ignore the fullscreen hint. */
	long motif[5] = { 2 /* MWM_HINTS_DECORATIONS */, 0, 0, 0, 0 };
	Atom mwm = XInternAtom(A.dpy, "_MOTIF_WM_HINTS", False);
	XChangeProperty(A.dpy, A.win, mwm, mwm, 32, PropModeReplace, (unsigned char *)motif, 5);

	A.wm_delete = XInternAtom(A.dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(A.dpy, A.win, &A.wm_delete, 1);

	A.gc = XCreateGC(A.dpy, A.win, 0, NULL);
	alloc_colors();
	resize_buffer(w, h);

	/* Blank cursor for idle hiding. */
	char zero = 0;
	XColor black = { 0 };
	Pixmap bm = XCreateBitmapFromData(A.dpy, A.win, &zero, 1, 1);
	A.blank = XCreatePixmapCursor(A.dpy, bm, bm, &black, &black, 0, 0);
	XFreePixmap(A.dpy, bm);

	/* Input method for UTF-8 text entry; fall back to XLookupString. */
	XSetLocaleModifiers("");
	A.xim = XOpenIM(A.dpy, NULL, NULL, NULL);
	if (A.xim)
		A.xic = XCreateIC(A.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
		                  XNClientWindow, A.win, XNFocusWindow, A.win, NULL);

	XMapRaised(A.dpy, A.win);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */

static void reload_config(int announce)
{
	int ok = config_load(A.cfg_path, &A.cfg) == 0;
	if (announce)
		set_status(ok ? "Config reloaded" : "Config not found \xC2\xB7 using defaults");
}

static void refresh_weather(void)
{
	weather_cancel(&A.wx);
	weather_start(&A.wx, A.cfg.latitude, A.cfg.longitude);
	A.dirty = 1;
}

static void do_poweroff(void)
{
	if (A.dry_run) {
		printf("t410dash: DRY RUN: would execute: doas /sbin/poweroff (peak %.1f C)\n",
		       A.th.peak_c);
		fflush(stdout);
		/* Stay usable for testing: behave as if snoozed. */
		thermal_snooze(&A.th, A.cfg.snooze_sec, time(NULL));
		set_status("Dry run \xC2\xB7 poweroff printed to stdout");
		return;
	}
	fprintf(stderr, "t410dash: peak %.1f C above limit, powering off\n", A.th.peak_c);
	pid_t pid = fork();
	if (pid == 0) {
		execlp("doas", "doas", "/sbin/poweroff", (char *)NULL);
		_exit(127);
	}
	A.poweroff_pid = pid > 0 ? pid : 0;
}

static void start_edit(void)
{
	A.editing = 1;
	snprintf(A.edit, sizeof(A.edit), "%s", A.cfg.task);
	A.edit_len = strlen(A.edit);
	A.dirty = 1;
}

static void finish_edit(int save)
{
	A.editing = 0;
	A.dirty = 1;
	if (!save)
		return;
	if (config_save_task(A.cfg_path, A.edit) == 0) {
		snprintf(A.cfg.task, sizeof(A.cfg.task), "%s", A.edit);
		set_status("Task saved");
	} else {
		set_status("Could not save task");
	}
}

static void edit_key(KeySym ks, const char *str, int len)
{
	if (ks == XK_Return || ks == XK_KP_Enter) {
		finish_edit(1);
	} else if (ks == XK_Escape) {
		finish_edit(0);
	} else if (ks == XK_BackSpace) {
		while (A.edit_len > 0) {
			unsigned char c = (unsigned char)A.edit[--A.edit_len];
			if ((c & 0xC0) != 0x80)
				break;
		}
		A.edit[A.edit_len] = '\0';
	} else if (len > 0 && (unsigned char)str[0] >= 0x20 && str[0] != 0x7f) {
		if (A.edit_len + (size_t)len < sizeof(A.edit)) {
			memcpy(A.edit + A.edit_len, str, (size_t)len);
			A.edit_len += (size_t)len;
			A.edit[A.edit_len] = '\0';
		}
	}
	A.dirty = 1;
}

static void handle_key(XKeyEvent *ev)
{
	char str[32];
	KeySym ks = NoSymbol;
	int len;

	if (A.xic) {
		Status st;
		len = Xutf8LookupString(A.xic, ev, str, sizeof(str) - 1, &ks, &st);
		if (st == XBufferOverflow)
			len = 0;
	} else {
		len = XLookupString(ev, str, sizeof(str) - 1, &ks, NULL);
	}
	if (len < 0)
		len = 0;
	str[len] = '\0';

	/* Any key during the countdown cancels it (q included). */
	if (A.th.state == TH_COUNTDOWN) {
		thermal_snooze(&A.th, A.cfg.snooze_sec, time(NULL));
		set_status("Shutdown cancelled \xC2\xB7 snoozed");
		return;
	}
	if (A.th.state == TH_POWEROFF)
		return;

	if (A.editing) {
		edit_key(ks, str, len);
		return;
	}
	if (len != 1)
		return;

	int c = tolower((unsigned char)str[0]);
	if (c == tolower((unsigned char)A.cfg.toggle_key[0])) {
		A.bank_revealed = !A.bank_revealed;
		A.dirty = 1;
	} else if (c == 'e') {
		start_edit();
	} else if (c == 'r') {
		reload_config(1);
		refresh_weather();
	} else if (c == 'q') {
		running = 0;
	}
}

static void handle_event(XEvent *ev)
{
	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0 && A.buf)
			XCopyArea(A.dpy, A.buf, A.win, A.gc, 0, 0, (unsigned)A.w, (unsigned)A.h, 0, 0);
		break;
	case ConfigureNotify:
		resize_buffer(ev->xconfigure.width, ev->xconfigure.height);
		break;
	case MapNotify:
		XSetInputFocus(A.dpy, A.win, RevertToParent, CurrentTime);
		break;
	case KeyPress:
		handle_key(&ev->xkey);
		break;
	case MotionNotify:
		A.last_motion_ms = mono_ms();
		if (A.cursor_hidden) {
			XUndefineCursor(A.dpy, A.win);
			A.cursor_hidden = 0;
		}
		break;
	case ClientMessage:
		if ((Atom)ev->xclient.data.l[0] == A.wm_delete && A.th.state != TH_COUNTDOWN)
			running = 0;
		break;
	}
}

static void tick(time_t now)
{
	if (now >= A.next_thermal) {
		A.th.peak_c = thermal_read_peak(A.fake_temp);
		A.next_thermal = now + 2;
	}
	if (thermal_update(&A.th, A.th.peak_c, A.cfg.high_temp_c, A.cfg.shutdown_delay_sec, now))
		do_poweroff();

	if (weather_tick(&A.wx, now))
		A.dirty = 1;
	if (A.wx.pid == 0 && now >= A.wx.next_fetch)
		weather_start(&A.wx, A.cfg.latitude, A.cfg.longitude);

	if (A.poweroff_pid > 0 && waitpid(A.poweroff_pid, NULL, WNOHANG) == A.poweroff_pid)
		A.poweroff_pid = 0;

	if (!A.cursor_hidden && mono_ms() - A.last_motion_ms > CURSOR_IDLE_MS) {
		XDefineCursor(A.dpy, A.win, A.blank);
		A.cursor_hidden = 1;
	}
	A.dirty = 1; /* clock */
}

static void usage(FILE *f)
{
	fprintf(f,
	        "usage: t410dash [options] [config.json]\n"
	        "\n"
	        "  --dry-run              print instead of running doas /sbin/poweroff\n"
	        "                         (also: T410DASH_DRY_RUN=1)\n"
	        "  --fake-temp-file PATH  read the temperature (C or millidegrees) from PATH\n"
	        "                         instead of sysfs (also: T410DASH_FAKE_TEMP=PATH)\n"
	        "  -h, --help             show this help\n"
	        "  -v, --version          show version\n"
	        "\n"
	        "Config lookup: argument, ./config.json next to the binary,\n"
	        "then ~/.config/t410dash/config.json.\n");
}

int main(int argc, char **argv)
{
	const char *cfg_arg = NULL;
	const char *env;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dry-run")) {
			A.dry_run = 1;
		} else if (!strcmp(argv[i], "--fake-temp-file") && i + 1 < argc) {
			A.fake_temp = argv[++i];
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			puts("t410dash " VERSION);
			return 0;
		} else if (argv[i][0] == '-') {
			usage(stderr);
			return 2;
		} else {
			cfg_arg = argv[i];
		}
	}
	if ((env = getenv("T410DASH_DRY_RUN")) && *env && strcmp(env, "0"))
		A.dry_run = 1;
	if (!A.fake_temp && (env = getenv("T410DASH_FAKE_TEMP")) && *env)
		A.fake_temp = env;

	setlocale(LC_CTYPE, "");
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	config_resolve_path(cfg_arg, argv[0], A.cfg_path, sizeof(A.cfg_path));
	if (config_load(A.cfg_path, &A.cfg) != 0)
		fprintf(stderr, "t410dash: %s not readable, using defaults\n", A.cfg_path);
	if (A.dry_run)
		fprintf(stderr, "t410dash: dry run, poweroff will only be printed\n");

	A.dpy = XOpenDisplay(NULL);
	if (!A.dpy) {
		fprintf(stderr, "t410dash: cannot open display\n");
		return 1;
	}
	fcntl(ConnectionNumber(A.dpy), F_SETFD, FD_CLOEXEC);

	weather_init(&A.wx);
	A.th.peak_c = -1;
	A.last_motion_ms = mono_ms();
	create_window();
	weather_start(&A.wx, A.cfg.latitude, A.cfg.longitude);

	time_t last = 0;
	while (running) {
		while (running && XPending(A.dpy)) {
			XEvent ev;
			XNextEvent(A.dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			handle_event(&ev);
		}

		time_t now = time(NULL);
		if (now != last) {
			last = now;
			tick(now);
		}
		if (A.dirty) {
			render();
			A.dirty = 0;
		}
		XFlush(A.dpy);

		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		int timeout = 1000 - (int)(ts.tv_nsec / 1000000) + 2;

		struct pollfd pfd[2] = {
			{ ConnectionNumber(A.dpy), POLLIN, 0 },
			{ A.wx.fd, POLLIN, 0 },
		};
		int n = poll(pfd, A.wx.fd >= 0 ? 2 : 1, timeout);
		if (n < 0 && errno != EINTR)
			break;
		if (n > 0 && A.wx.fd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
			if (weather_read(&A.wx))
				A.dirty = 1;
		}
	}

	weather_cancel(&A.wx);
	close_fonts();
	if (A.xic)
		XDestroyIC(A.xic);
	if (A.xim)
		XCloseIM(A.xim);
	XftDrawDestroy(A.draw);
	XFreePixmap(A.dpy, A.buf);
	XFreeCursor(A.dpy, A.blank);
	XFreeGC(A.dpy, A.gc);
	XDestroyWindow(A.dpy, A.win);
	XCloseDisplay(A.dpy);
	return 0;
}
