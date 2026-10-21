/*
 * utr-lcd-status - render router status on the UTR-LR LCD (built-in renderer).
 *
 * A screen is a vertical stack of widgets (SSID, signal, clients, IPs, VPN, uptime, ...) drawn over
 * an optional Wi-Fi signal backfill and topped by a status indicator. Several screens can be
 * configured; they rotate on their own dwell times in playlist order, or one is pinned. The layout
 * comes from a flat key=value file (see conf_parse, written by the init script from /etc/config/utr-lcd);
 * without that file the built-in defaults are used, so the renderer works standalone.
 *
 * Collected data (key=value lines, see collect.sh) is refreshed in the background; the panel is
 * redrawn only when what is shown changes.
 *
 * Layout is designed for the measured visible window of the glass (docs/decisions.md: the bezel hides a
 * few px at the sides and rounds the corners with ~31 px radius) and for off-axis viewing: black
 * background, large bold text, every state spelled out in words (never colour alone).
 *
 * Usage: utr-lcd-status [-c collector] [-C screens.conf] [-i seconds] [-0] [-1 [-N screen]] [-l]
 *   -c  collector command   (default /usr/libexec/utr-lcd/collect.sh)
 *   -C  screen layout file  (default /var/run/utr-lcd/screens.conf)
 *   -i  refresh interval    (default 5)
 *   -0  do not rotate 180 degrees (default rotates; the panel is mounted upside-down)
 *   -1  draw once, hold, exit (for testing; hold time is LCD_ONCE_HOLD seconds)
 *   -N  with -1: which screen to draw (1-based, default 1)
 *   -l  list the configured screens (number, enabled, dwell, name) and exit
 * Env: LCD_TRACE=1 prints a line (ms, screen number) to stderr whenever a screen is drawn.
 */
#include <poll.h>
#include <signal.h>
#include <time.h>
#include "lcd.h"

#ifndef LCD_ONCE_HOLD
#define LCD_ONCE_HOLD 30
#endif

#define C_BG      LCD_BLACK
#define C_WHITE   LCD_WHITE
#define C_DIM     RGB(140, 160, 195)
#define C_RULE    RGB(45, 55, 80)
#define C_PILL    RGB(30, 38, 58)
#define C_OK      RGB(40, 225, 105)
#define C_WARN    RGB(255, 190, 30)
#define C_BAD     RGB(255, 75, 75)

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static void copy(char *dst, size_t n, const char *src)
{
	size_t len = strlen(src);
	if (len >= n) len = n - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------------------------------ data */

typedef struct {
	char up_state[16], up_type[16], up_name[64], up_detail[64], wan_ip[48], lan_ip[48];
	char vpn_state[16], vpn_name[48], inet[8];
	int signal, has_signal, speed, clients;
	long uptime;
	int valid;
} status_t;

static void collect_parse(status_t *s, char *line)
{
	char *eq = strchr(line, '=');
	if (!eq) return;
	*eq++ = 0;
	if (!strcmp(line, "up_state")) copy(s->up_state, sizeof(s->up_state), eq);
	else if (!strcmp(line, "up_type")) copy(s->up_type, sizeof(s->up_type), eq);
	else if (!strcmp(line, "up_name")) copy(s->up_name, sizeof(s->up_name), eq);
	else if (!strcmp(line, "up_detail")) copy(s->up_detail, sizeof(s->up_detail), eq);
	else if (!strcmp(line, "up_signal")) { s->has_signal = *eq != 0; s->signal = atoi(eq); }
	else if (!strcmp(line, "up_speed")) s->speed = atoi(eq);
	else if (!strcmp(line, "wan_ip")) copy(s->wan_ip, sizeof(s->wan_ip), eq);
	else if (!strcmp(line, "lan_ip")) copy(s->lan_ip, sizeof(s->lan_ip), eq);
	else if (!strcmp(line, "vpn_state")) copy(s->vpn_state, sizeof(s->vpn_state), eq);
	else if (!strcmp(line, "vpn_name")) copy(s->vpn_name, sizeof(s->vpn_name), eq);
	else if (!strcmp(line, "inet")) copy(s->inet, sizeof(s->inet), eq);
	else if (!strcmp(line, "clients")) s->clients = atoi(eq);
	else if (!strcmp(line, "uptime")) s->uptime = atol(eq);
	else return;
	s->valid = 1;
}

/* The collector runs in the background (it pings, so it can take a second); its output is read
 * without blocking so that screens keep rotating on time. */
typedef struct {
	FILE *p;
	int fd;
	char buf[2048];
	size_t len;
	status_t next;
} collector_t;

static void coll_start(collector_t *c, const char *cmd)
{
	c->p = popen(cmd, "r");
	if (!c->p) return;
	c->fd = fileno(c->p);
	c->len = 0;
	memset(&c->next, 0, sizeof(c->next));
}

/* Returns 1 when a complete result is in *out. */
static int coll_poll(collector_t *c, status_t *out)
{
	if (!c->p) return 0;
	struct pollfd pf = { .fd = c->fd, .events = POLLIN };
	while (poll(&pf, 1, 0) > 0) {
		ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
		if (n > 0) {
			c->len += (size_t)n;
			char *nl;
			c->buf[c->len] = 0;
			while ((nl = strchr(c->buf, '\n'))) {
				*nl = 0;
				collect_parse(&c->next, c->buf);
				c->len -= (size_t)(nl + 1 - c->buf);
				memmove(c->buf, nl + 1, c->len + 1);
			}
			if (c->len >= sizeof(c->buf) - 1) c->len = 0;   /* runaway line */
			continue;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		pclose(c->p);
		c->p = NULL;
		*out = c->next;
		return 1;
	}
	return 0;
}

/* ---------------------------------------------------------------------------------- configuration */

enum { W_SSID, W_DBM, W_CLIENTS, W_WANIP, W_LANIP, W_VPN, W_UPTIME, W_RULE, W_NTYPES };
enum { ST_DOT, ST_PILL, ST_NONE };

typedef struct { const char *name; int def_size; } wtype_t;
static const wtype_t WTYPES[W_NTYPES] = {
	[W_SSID]    = { "ssid", 24 },
	[W_DBM]     = { "dbm", 32 },
	[W_CLIENTS] = { "clients", 24 },
	[W_WANIP]   = { "wanip", 16 },
	[W_LANIP]   = { "lanip", 16 },
	[W_VPN]     = { "vpn", 16 },
	[W_UPTIME]  = { "uptime", 16 },
	[W_RULE]    = { "rule", 2 },
};

#define MAX_WIDGETS 16
#define MAX_SCREENS 12
#define MAX_PLAYLIST 32

typedef struct { int type, size; } widget_t;
typedef struct {
	char name[24];
	int dwell_ms, enabled, indicator, backfill;
	widget_t w[MAX_WIDGETS];
	int nw;
} screen_t;
typedef struct {
	screen_t s[MAX_SCREENS];
	int ns;
	int pinned, pin;                   /* mode=pin: show only screen `pin` (1-based) */
	int playlist[MAX_PLAYLIST], npl;   /* 1-based screen numbers; empty = every enabled screen in order */
	int fill_dim, fill_edge;           /* signal backfill: brightness percent, bright edge line */
} config_t;

static const char DEFAULT_CONF[] =
	"fill_dim=30\nfill_edge=1\n"
	"screen=Status\ndwell=8\nindicator=dot\nbackfill=1\nwidget=ssid:24\nwidget=dbm:32\nwidget=clients:24\n"
	"screen=Details\ndwell=5\nindicator=dot\nbackfill=1\n"
	"widget=wanip:16\nwidget=lanip:16\nwidget=vpn:16\nwidget=uptime:16\n";

static void conf_line(config_t *c, char *line)
{
	char *eq = strchr(line, '=');
	if (!eq) return;
	*eq++ = 0;
	screen_t *s = c->ns ? &c->s[c->ns - 1] : NULL;
	if (!strcmp(line, "screen")) {
		if (c->ns >= MAX_SCREENS) return;
		s = &c->s[c->ns++];
		memset(s, 0, sizeof(*s));
		copy(s->name, sizeof(s->name), eq);
		s->dwell_ms = 8000; s->enabled = 1; s->indicator = ST_DOT; s->backfill = 1;
	} else if (!s) {
		if (!strcmp(line, "mode")) c->pinned = !strcmp(eq, "pin");
		else if (!strcmp(line, "pin")) c->pin = atoi(eq);
		else if (!strcmp(line, "fill_dim")) c->fill_dim = atoi(eq);
		else if (!strcmp(line, "fill_edge")) c->fill_edge = atoi(eq);
		else if (!strcmp(line, "playlist")) {
			for (char *t = strtok(eq, " ,"); t && c->npl < MAX_PLAYLIST; t = strtok(NULL, " ,"))
				if (atoi(t) > 0) c->playlist[c->npl++] = atoi(t);
		}
	} else if (!strcmp(line, "dwell")) {
		int ms = (int)(atof(eq) * 1000);
		s->dwell_ms = ms < 500 ? 500 : ms > 3600000 ? 3600000 : ms;       /* 0.5 s .. 1 h */
	} else if (!strcmp(line, "enabled")) s->enabled = atoi(eq) != 0;
	else if (!strcmp(line, "indicator")) s->indicator = !strcmp(eq, "pill") ? ST_PILL : !strcmp(eq, "none") ? ST_NONE : ST_DOT;
	else if (!strcmp(line, "backfill")) s->backfill = atoi(eq) != 0;
	else if (!strcmp(line, "widget") && s->nw < MAX_WIDGETS) {
		char *colon = strchr(eq, ':');
		int size = 0;
		if (colon) { *colon++ = 0; size = atoi(colon); }
		for (int t = 0; t < W_NTYPES; t++)
			if (!strcmp(eq, WTYPES[t].name)) {
				s->w[s->nw].type = t;
				s->w[s->nw].size = size > 0 ? size : WTYPES[t].def_size;
				s->nw++;
				break;
			}
	}
}

static void conf_parse(config_t *c, const char *text)
{
	char line[128];
	while (*text) {
		size_t n = strcspn(text, "\n");
		if (n >= sizeof(line)) n = sizeof(line) - 1;
		memcpy(line, text, n);
		line[n] = 0;
		line[strcspn(line, "\r")] = 0;
		if (line[0] && line[0] != '#') conf_line(c, line);
		text += strcspn(text, "\n");
		if (*text) text++;
	}
}

static void conf_load(config_t *c, const char *path)
{
	memset(c, 0, sizeof(*c));
	c->fill_dim = 30; c->fill_edge = 1;
	char text[4096];
	FILE *f = path ? fopen(path, "r") : NULL;
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	if (f) fclose(f);
	text[n] = 0;
	if (n) conf_parse(c, text);
	if (!c->ns) {                    /* missing or empty: built-in screens */
		int fd = c->fill_dim, fe = c->fill_edge;
		memset(c, 0, sizeof(*c));
		c->fill_dim = fd; c->fill_edge = fe;
		conf_parse(c, DEFAULT_CONF);
	}
	if (c->fill_dim < 5) c->fill_dim = 5;
	if (c->fill_dim > 100) c->fill_dim = 100;
}

/* Screens to show, in order: the pinned one, or the playlist, or every enabled screen. */
static int build_order(const config_t *c, int *order)
{
	int n = 0;
	if (c->pinned) {
		if (c->pin >= 1 && c->pin <= c->ns) order[n++] = c->pin - 1;
	} else if (c->npl) {
		for (int i = 0; i < c->npl && n < MAX_PLAYLIST; i++)
			if (c->playlist[i] <= c->ns && c->s[c->playlist[i] - 1].enabled) order[n++] = c->playlist[i] - 1;
	} else {
		for (int i = 0; i < c->ns; i++) if (c->s[i].enabled) order[n++] = i;
	}
	if (!n) order[n++] = 0;
	return n;
}

/* --------------------------------------------------------------------------------------- geometry */

/* Visible window of the glass in logical (viewer) coordinates, and where content may go. */
typedef struct {
	int x0, x1, y0, y1;   /* visible window */
	int r;                /* corner radius */
	int ml, mr;           /* side margins inside the window */
	int cx;               /* horizontal centre of the content */
} win_t;

/* How far the rounded corners cut into the window at row y. */
static int corner_inset(const win_t *w, int y)
{
	int d = y - w->y0;
	if (w->y1 - y < d) d = w->y1 - y;
	if (d >= w->r) return 0;
	if (d < 0) return w->r;
	for (int i = 0; i < w->r; i++) {
		int a = 2 * w->r - 2 * i - 1, b = 2 * w->r - 2 * d - 1;
		if (a * a + b * b <= 4 * w->r * w->r) return i;
	}
	return w->r;
}

typedef struct { int x0, x1; } span_t;

/* Horizontal room for content spanning rows y..y+h-1. */
static span_t span_for(const win_t *w, int y, int h)
{
	int a = corner_inset(w, y), b = corner_inset(w, y + h - 1), in = a > b ? a : b;
	span_t s = { w->x0 + (in + 4 > w->ml ? in + 4 : w->ml), w->x1 - (in + 4 > w->mr ? in + 4 : w->mr) };
	return s;
}

/* ------------------------------------------------------------------------------------- primitives */

static const char *ICON_USER[16] = {
	"................", "......####......", ".....######.....", ".....######.....", ".....######.....", "......####......",
	"................", "....########....", "..############..", ".##############.", ".##############.", ".##############.",
	".##############.", "................", "................", "................" };
static const char *ICON_SHIELD[16] = {
	"..############..", ".##############.", ".##############.", ".##############.", ".######..######.", ".#####....#####.",
	".######..######.", ".######..######.", "..############..", "..############..", "...##########...", "....########....",
	".....######.....", "......####......", ".......##.......", "................" };

static void draw_icon(lcd_t *l, int x, int y, const char *const *rows, uint32_t c, int sc)
{
	for (int j = 0; j < 16; j++)
		for (int i = 0; i < 16; i++)
			if (rows[j][i] == '#') lcd_fill(l, x + i * sc, y + j * sc, sc, sc, c);
}

/* Text ladder, large to small: sizes in pixels of cell height. */
static const font_t *LADDER[] = { &FONT_T32B, &FONT_T24B, &FONT_T16B, &FONT_T12 };
#define NLADDER 4
static int ladder_index(int size) { return size >= 32 ? 0 : size >= 24 ? 1 : size >= 16 ? 2 : 3; }

/* Shorten s to fit maxw pixels in f: ".." at the end, or in the middle (addresses). */
static void ellipsize(const font_t *f, const char *s, int maxw, int middle, char *out, size_t n)
{
	int maxc = maxw / f->w, len = (int)strlen(s);
	if (maxc < 3) maxc = 3;
	if (len <= maxc) { copy(out, n, s); return; }
	if (middle) {
		int head = (maxc - 2 + 1) / 2, tail = maxc - 2 - head;
		snprintf(out, n, "%.*s..%s", head, s, s + len - tail);
	} else {
		snprintf(out, n, "%.*s..", maxc - 2, s);
	}
}

/* Largest ladder font from `start` down in which s fits maxw; the last one is ellipsized. */
static const font_t *fit_font(int start, const char *s, int maxw)
{
	int i = start;
	while (i < NLADDER - 1 && lcd_text_w(LADDER[i], s, 1) > maxw) i++;
	return LADDER[i];
}

static void text_centered(lcd_t *l, const win_t *w, span_t sp, int y, int h, int size, const char *s, uint32_t c, int middle)
{
	char buf[64];
	int half = sp.x1 - w->cx < w->cx - sp.x0 ? sp.x1 - w->cx : w->cx - sp.x0, maxw = 2 * half + 1;
	const font_t *f = fit_font(ladder_index(size), s, maxw);
	ellipsize(f, s, maxw, middle, buf, sizeof(buf));
	lcd_draw(l, f, w->cx - lcd_text_w(f, buf, 1) / 2, y + (h - f->h) / 2, buf, c, LCD_TRANSPARENT, 1);
}

/* Label on the left, value on the right. Text steps down from 8x16 to 6x12 before the value is cut;
 * *step is the smallest step to use (so that every row of a screen matches) and returns the step used.
 * l == NULL only measures. */
static void row_text(lcd_t *l, span_t sp, int y, int h, int size, const char *label, const char *value,
		     uint32_t vc, int middle, const char *const *icon, uint32_t icon_c, int *step)
{
	static const font_t *LAB[] = { &FONT_T16, &FONT_T12 }, *VAL[] = { &FONT_T16B, &FONT_T12 };
	int st = *step > (size >= 16 ? 0 : 1) ? *step : size >= 16 ? 0 : 1, width = sp.x1 - sp.x0 + 1, lw0 = icon ? 20 : 0;
	char buf[64];
	if (st == 0 && lw0 + lcd_text_w(LAB[0], label, 1) + 4 + lcd_text_w(VAL[0], value, 1) > width) st = 1;
	*step = st;
	if (!l) return;
	int lw = lw0 + lcd_text_w(LAB[st], label, 1);
	ellipsize(VAL[st], value, width - lw - 4, middle, buf, sizeof(buf));
	if (icon) draw_icon(l, sp.x0, y + (h - 16) / 2, icon, icon_c, 1);
	lcd_draw(l, LAB[st], sp.x0 + lw0, y + (h - LAB[st]->h) / 2, label, C_DIM, LCD_TRANSPARENT, 1);
	lcd_draw(l, VAL[st], sp.x1 + 1 - lcd_text_w(VAL[st], buf, 1), y + (h - VAL[st]->h) / 2, buf, vc, LCD_TRANSPARENT, 1);
}

static void fmt_uptime(long t, char *out, size_t n)
{
	long d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60;
	if (d > 0) snprintf(out, n, "%ldd %ldh %02ldm", d, h, m);
	else if (h > 0) snprintf(out, n, "%ldh %02ldm", h, m);
	else snprintf(out, n, "%ldm %02lds", m, t % 60);
}

/* Wi-Fi quality 0..100 from dBm: -50 or better is full, -90 or worse empty. */
static int sig_pct(int dbm) { int p = (dbm + 90) * 100 / 40; return p < 0 ? 0 : p > 100 ? 100 : p; }

static uint32_t scale_rgb(int r, int g, int b, int pct) { return RGB(r * pct / 100, g * pct / 100, b * pct / 100); }

/* Vertical backfill from the bottom: red when the signal is poor, green when it is full. */
static void draw_backfill(lcd_t *l, int pct, int dim, int edge)
{
	int r, g, b = 0;
	if (pct < 50) { r = 255; g = pct * 51 / 10; } else { r = 255 - (pct - 50) * 51 / 10; g = 235; }
	int top = l->h - l->h * pct / 100;
	lcd_fill(l, 0, top, l->w, l->h - top, scale_rgb(r, g, b, dim));
	if (edge && pct > 0 && pct < 100) lcd_fill(l, 0, top, l->w, 3, scale_rgb(r, g, b, dim + 22 > 100 ? 100 : dim + 22));
}

static void draw_pill(lcd_t *l, const win_t *w, int y, const char *text, uint32_t dot)
{
	int tw = lcd_text_w(&FONT_T16B, text, 1), pw = 20 + tw + 6, x = w->cx - pw / 2;
	lcd_rrect(l, x, y, pw, 22, 11, C_PILL);
	lcd_disc(l, x + 11, y + 11, 4, dot);
	lcd_draw(l, &FONT_T16B, x + 20, y + 3, text, C_WHITE, LCD_TRANSPARENT, 1);
}

/* ---------------------------------------------------------------------------------------- widgets */

typedef struct { const status_t *s; const char *state; int backfill; int row_step; } ctx_t;

#define PAD 2
#define RULE_H 6

/* Height a widget needs; 0 = nothing to show right now (it takes no room). */
static int widget_h(const widget_t *wd, const ctx_t *c)
{
	switch (wd->type) {
	case W_DBM: {
		int wifi = c->s->has_signal && !strcmp(c->s->up_type, "wifi");
		return !strcmp(c->s->up_state, "up") && (wifi || c->s->speed > 0) ? wd->size + PAD : 0;
	}
	case W_WANIP: return c->s->wan_ip[0] ? wd->size + PAD : 0;
	case W_LANIP: return c->s->lan_ip[0] ? wd->size + PAD : 0;
	case W_VPN: return strcmp(c->s->vpn_state, "none") && c->s->vpn_state[0] ? wd->size + PAD : 0;
	case W_RULE: return RULE_H;
	default: return wd->size + PAD;
	}
}

static void widget_draw(lcd_t *l, const win_t *w, const widget_t *wd, ctx_t *c, int y, int h)
{
	const status_t *s = c->s;
	int up = !strcmp(s->up_state, "up"), wifi = !strcmp(s->up_type, "wifi");
	span_t sp = span_for(w, y, h);
	char buf[64];

	switch (wd->type) {
	case W_SSID:
		if (!up) text_centered(l, w, sp, y, h, wd->size, "No uplink", C_BAD, 0);
		else if (wifi) text_centered(l, w, sp, y, h, wd->size, s->up_name[0] ? s->up_name : "Wi-Fi", C_WHITE, 0);
		else text_centered(l, w, sp, y, h, wd->size, !strcmp(s->up_type, "usb") ? "USB WAN" : "Ethernet", C_WHITE, 0);
		break;
	case W_DBM:
		if (wifi && s->has_signal) {
			int p = sig_pct(s->signal);
			snprintf(buf, sizeof(buf), "%d dBm", s->signal);
			text_centered(l, w, sp, y, h, wd->size, buf, c->backfill ? C_WHITE : p >= 50 ? C_OK : p >= 25 ? C_WARN : C_BAD, 0);
		} else {
			snprintf(buf, sizeof(buf), "%d Mbps", s->speed);
			text_centered(l, w, sp, y, h, wd->size, buf, C_WHITE, 0);
		}
		break;
	case W_CLIENTS: {
		const font_t *f = LADDER[ladder_index(wd->size)];
		int sc = wd->size >= 32 ? 2 : 1, iw = 16 * sc;
		snprintf(buf, sizeof(buf), "%d", s->clients);
		int tw = lcd_text_w(f, buf, 1), x = w->cx - (iw + 6 + tw) / 2;
		draw_icon(l, x, y + (h - iw) / 2, ICON_USER, C_WHITE, sc);
		lcd_draw(l, f, x + iw + 6, y + (h - f->h) / 2, buf, C_WHITE, LCD_TRANSPARENT, 1);
		break;
	}
	case W_WANIP: row_text(l, sp, y, h, wd->size, "WAN", s->wan_ip, C_WHITE, 1, NULL, 0, &c->row_step); break;
	case W_LANIP: row_text(l, sp, y, h, wd->size, "LAN", s->lan_ip, C_WHITE, 1, NULL, 0, &c->row_step); break;
	case W_VPN: {
		int vup = !strcmp(s->vpn_state, "up");
		const char *nm = s->vpn_name[0] ? s->vpn_name : "VPN";
		char buf[96];
		if (vup) copy(buf, sizeof(buf), nm);
		else snprintf(buf, sizeof(buf), "%s %s", nm, !strcmp(s->vpn_state, "connecting") ? "..." : "off");
		uint32_t col = vup ? C_OK : !strcmp(s->vpn_state, "connecting") ? C_WARN : C_BAD;
		row_text(l, sp, y, h, wd->size, "", buf, C_WHITE, 0, ICON_SHIELD, col, &c->row_step);
		break;
	}
	case W_UPTIME:
		fmt_uptime(s->uptime, buf, sizeof(buf));
		row_text(l, sp, y, h, wd->size, "Uptime", buf, C_WHITE, 0, NULL, 0, &c->row_step);
		break;
	case W_RULE:
		lcd_fill(l, sp.x0, y + h / 2 - 1, sp.x1 - sp.x0 + 1, 2, C_RULE);
		break;
	}
}

/* ----------------------------------------------------------------------------------------- screen */

static void draw_screen(lcd_t *l, const win_t *w, const config_t *cf, const screen_t *sc, const status_t *s, const char *state)
{
	ctx_t c = { s, state, sc->backfill, 0 };
	lcd_fill(l, 0, 0, l->w, l->h, C_BG);

	if (!s->valid) {
		draw_pill(l, w, 6, "NO DATA", C_BAD);
		lcd_flush(l);
		return;
	}

	int up = !strcmp(s->up_state, "up");
	if (sc->backfill && up && !strcmp(s->up_type, "wifi") && s->has_signal)
		draw_backfill(l, sig_pct(s->signal), cf->fill_dim, cf->fill_edge);

	/* status indicator: dot, grown into a text banner when something is wrong */
	int bad = strcmp(state, "ready") != 0, top = w->y0 + 4;
	uint32_t col = !strcmp(state, "noInternet") ? C_WARN : bad ? C_BAD : C_OK;
	const char *text = !strcmp(state, "noUplink") ? "NO UPLINK" : !strcmp(state, "noInternet") ? "NO INTERNET" : "ONLINE";
	if (sc->indicator == ST_PILL || (sc->indicator == ST_DOT && bad)) {
		draw_pill(l, w, 6, text, col);
		top = 6 + 22 + 4;
	} else if (sc->indicator == ST_DOT) {
		lcd_disc(l, w->cx, 10, 5, col);
		top = 10 + 5 + 4;
	}

	/* widgets: stacked, spare room spread evenly (at most 14 px between them) */
	int hs[MAX_WIDGETS], nvis = 0, total = 0, bottom = w->y1 - 3;
	for (int i = 0; i < sc->nw; i++) {
		hs[i] = widget_h(&sc->w[i], &c);
		if (hs[i]) { nvis++; total += hs[i]; }
	}
	for (int i = 0; i < sc->nw; i++) {        /* measure: all rows of a screen share one text size */
		int t = sc->w[i].type;
		if (hs[i] && (t == W_WANIP || t == W_LANIP || t == W_VPN || t == W_UPTIME)) widget_draw(NULL, w, &sc->w[i], &c, top, hs[i]);
	}
	if (nvis) {
		int spare = bottom - top - total, gap = nvis > 1 ? spare / (nvis + 1) : spare / 2;
		if (gap < 2) gap = 2;
		if (gap > 14) gap = 14;
		int y = top + (spare - gap * (nvis - 1)) / 2;
		if (y < top) y = top;
		for (int i = 0; i < sc->nw; i++) {
			if (!hs[i]) continue;
			widget_draw(l, w, &sc->w[i], &c, y, hs[i]);
			y += hs[i] + gap;
		}
	}
	lcd_flush(l);
}

/* Is the backlight off? Rotation pauses then, so the first screen after waking gets its full time. */
static int backlight_off(void)
{
	FILE *f = fopen("/sys/class/backlight/backlight/brightness", "r");
	int v = 1;
	if (f) { if (fscanf(f, "%d", &v) != 1) v = 1; fclose(f); }
	return v == 0;
}

int main(int argc, char **argv)
{
	const char *cmd = "/usr/libexec/utr-lcd/collect.sh", *conf = "/var/run/utr-lcd/screens.conf";
	int interval = 5, rotate = 1, once = 0, once_screen = 1, list = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc) cmd = argv[++i];
		else if (!strcmp(argv[i], "-C") && i + 1 < argc) conf = argv[++i];
		else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-N") && i + 1 < argc) once_screen = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-0")) rotate = 0;
		else if (!strcmp(argv[i], "-1")) once = 1;
		else if (!strcmp(argv[i], "-l")) list = 1;
		else {
			fprintf(stderr, "usage: %s [-c collector] [-C screens.conf] [-i sec] [-0] [-1 [-N screen]] [-l]\n", argv[0]);
			return 2;
		}
	}
	if (interval < 1) interval = 1;

	config_t cf;
	conf_load(&cf, conf);
	if (list) {
		for (int i = 0; i < cf.ns; i++) printf("%d %d %d %s\n", i + 1, cf.s[i].enabled, cf.s[i].dwell_ms, cf.s[i].name);
		return 0;
	}
	int order[MAX_PLAYLIST], norder = build_order(&cf, order), pos = 0;
	if (once && once_screen >= 1 && once_screen <= cf.ns) { order[0] = once_screen - 1; norder = 1; }

	struct sigaction sa = { .sa_handler = on_sig };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	lcd_t l;
	if (lcd_open(&l, "/dev/dri/card0", rotate, rotate)) return 1;

	/*
	 * Visible window of the glass, measured with a calibration grid photographed straight on (see
	 * docs/decisions.md): in the mounted orientation the bezel hides ~5 px at the viewer's left and
	 * ~1 px at the right, 2 at the top and 3 at the bottom, and rounds the corners (~31 px radius).
	 */
	win_t w = { .x0 = rotate ? 5 : 1, .x1 = l.w - 1 - (rotate ? 1 : 5), .y0 = rotate ? 2 : 3, .y1 = l.h - 1 - (rotate ? 3 : 2),
		    .r = 31, .ml = 8, .mr = 10 };
	w.cx = (w.x0 + w.ml + w.x1 - w.mr) / 2;

	collector_t coll = { 0 };
	status_t st = { 0 };
	char sig[512] = "", prev[512] = "";
	int fails = 0, drawn_pos = -1, have_data = 0;
	long next_collect = 0, switch_at = 0, bl_checked = 0;
	int dark = 0, trace = getenv("LCD_TRACE") != NULL;

	while (!stop) {
		long now = now_ms();

		if (!coll.p && now >= next_collect) coll_start(&coll, cmd);
		status_t fresh;
		if (coll_poll(&coll, &fresh)) {
			st = fresh;
			have_data = 1;
			next_collect = now_ms() + interval * 1000L;
			int up = !strcmp(st.up_state, "up");
			fails = (up && !strcmp(st.inet, "fail")) ? fails + 1 : 0;
		}
		if (!have_data && !coll.p && now >= next_collect + 10000) next_collect = now;   /* collector failed to start */

		if (now - bl_checked >= 500) { dark = backlight_off(); bl_checked = now; }

		/* screen rotation */
		if (drawn_pos < 0 || dark) switch_at = now + cf.s[order[pos]].dwell_ms;
		else if (norder > 1 && now >= switch_at) {
			pos = (pos + 1) % norder;
			switch_at = now + cf.s[order[pos]].dwell_ms;
		}

		if (have_data) {
			int up = !strcmp(st.up_state, "up");
			const char *state = !up ? "noUplink" : fails >= 2 ? "noInternet" : "ready";
			char up_s[32];
			fmt_uptime(st.uptime, up_s, sizeof(up_s));
			snprintf(sig, sizeof(sig), "%d|%s|%d|%s|%s|%s|%s|%s|%d|%d|%d|%s|%s|%s|%d|%s", pos, state, st.valid, st.up_type,
				 st.up_name, st.up_detail, st.wan_ip, st.lan_ip, st.has_signal, st.signal, st.speed, st.vpn_state,
				 st.vpn_name, st.inet, st.clients, up_s);
			if (pos != drawn_pos || strcmp(sig, prev)) {
				draw_screen(&l, &w, &cf, &cf.s[order[pos]], &st, state);
				if (trace) fprintf(stderr, "%ld screen %d\n", now, order[pos] + 1);
				drawn_pos = pos;
				copy(prev, sizeof(prev), sig);
			}
			if (once && have_data) { sleep(LCD_ONCE_HOLD); break; }
		}

		long wait = 100;
		if (norder > 1 && !dark && switch_at - now_ms() < wait) wait = switch_at - now_ms() > 1 ? switch_at - now_ms() : 1;
		if (coll.p) { struct pollfd pf = { .fd = coll.fd, .events = POLLIN }; poll(&pf, 1, (int)wait); }
		else { struct timespec ts = { 0, wait * 1000000L }; nanosleep(&ts, NULL); }
	}

	if (!once || LCD_ONCE_HOLD > 0) {      /* blank the panel on exit (not for zero-hold previews) */
		lcd_fill(&l, 0, 0, l.w, l.h, LCD_BLACK);
		lcd_flush(&l);
	}
	lcd_close(&l);
	return 0;
}
