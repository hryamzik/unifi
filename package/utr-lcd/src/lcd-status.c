/*
 * utr-lcd-status - render router status on the UTR-LR LCD (built-in renderer).
 *
 * Runs a collector (key=value lines, see collect.sh) every few seconds and redraws the 136x240
 * panel only when what is shown changes.
 *
 * Layout is designed for the measured visible window of the glass (the bezel hides ~5 px on one
 * side and rounds the corners) and for off-axis viewing: black background, large bold text, every
 * state spelled out in words (never colour alone), no hairlines.
 *
 * Usage: utr-lcd-status [-c collector] [-i seconds] [-0] [-1]
 *   -c  collector command   (default /usr/libexec/utr-lcd/collect.sh)
 *   -i  refresh interval    (default 5)
 *   -0  do not rotate 180 degrees (default rotates; the panel is mounted upside-down)
 *   -1  draw once, hold, exit (for testing; hold time is LCD_ONCE_HOLD seconds)
 */
#include <signal.h>
#include "lcd.h"

#ifndef LCD_ONCE_HOLD
#define LCD_ONCE_HOLD 30
#endif

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

typedef struct {
	char up_state[16], up_type[16], up_name[64], up_detail[64], wan_ip[48];
	char vpn_state[16], vpn_name[48], inet[8];
	int signal, has_signal, speed, clients;
	long uptime;
	int valid;
} status_t;

/* Where content may be drawn, in logical (viewer) coordinates. */
typedef struct { int x0, x1, cx; } geom_t;

#define C_BG      LCD_BLACK
#define C_WHITE   LCD_WHITE
#define C_DIM     RGB(140, 160, 195)
#define C_RULE    RGB(45, 55, 80)
#define C_PILL    RGB(30, 38, 58)
#define C_OK      RGB(40, 225, 105)
#define C_WARN    RGB(255, 190, 30)
#define C_BAD     RGB(255, 75, 75)
#define C_UNLIT   RGB(55, 62, 82)

static void copy(char *dst, size_t n, const char *src)
{
	size_t len = strlen(src);
	if (len >= n) len = n - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
}

static int collect(const char *cmd, status_t *s)
{
	memset(s, 0, sizeof(*s));
	FILE *p = popen(cmd, "r");
	if (!p) return -1;
	char buf[256];
	while (fgets(buf, sizeof(buf), p)) {
		buf[strcspn(buf, "\r\n")] = 0;
		char *eq = strchr(buf, '=');
		if (!eq) continue;
		*eq++ = 0;
		if (!strcmp(buf, "up_state")) copy(s->up_state, sizeof(s->up_state), eq);
		else if (!strcmp(buf, "up_type")) copy(s->up_type, sizeof(s->up_type), eq);
		else if (!strcmp(buf, "up_name")) copy(s->up_name, sizeof(s->up_name), eq);
		else if (!strcmp(buf, "up_detail")) copy(s->up_detail, sizeof(s->up_detail), eq);
		else if (!strcmp(buf, "up_signal")) { s->has_signal = *eq != 0; s->signal = atoi(eq); }
		else if (!strcmp(buf, "up_speed")) s->speed = atoi(eq);
		else if (!strcmp(buf, "wan_ip")) copy(s->wan_ip, sizeof(s->wan_ip), eq);
		else if (!strcmp(buf, "vpn_state")) copy(s->vpn_state, sizeof(s->vpn_state), eq);
		else if (!strcmp(buf, "vpn_name")) copy(s->vpn_name, sizeof(s->vpn_name), eq);
		else if (!strcmp(buf, "inet")) copy(s->inet, sizeof(s->inet), eq);
		else if (!strcmp(buf, "clients")) s->clients = atoi(eq);
		else if (!strcmp(buf, "uptime")) s->uptime = atol(eq);
		else continue;
		s->valid = 1;
	}
	pclose(p);
	return s->valid ? 0 : -1;
}

static void fmt_uptime(long t, char *out, size_t n)
{
	long d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60;
	if (d > 0) snprintf(out, n, "%ldd %ldh %02ldm", d, h, m);
	else if (h > 0) snprintf(out, n, "%ldh %02ldm", h, m);
	else snprintf(out, n, "%ldm %02lds", m, t % 60);
}

/* 0..5 bars from the Wi-Fi signal in dBm. */
static int signal_bars(int dbm)
{
	return dbm >= -55 ? 5 : dbm >= -63 ? 4 : dbm >= -70 ? 3 : dbm >= -78 ? 2 : dbm >= -88 ? 1 : 0;
}

/* Copy s into out, shortened with ".." to fit maxw pixels in font f. */
static void fit(const font_t *f, const char *s, int maxw, char *out, size_t n)
{
	int maxc = maxw / f->w;
	if (maxc < 3) maxc = 3;
	if ((int)strlen(s) <= maxc) { copy(out, n, s); return; }
	snprintf(out, n, "%.*s..", maxc - 2, s);
}

static void text_center(lcd_t *l, const geom_t *g, const font_t *f, int y, const char *s, uint32_t c)
{
	char buf[64];
	fit(f, s, g->x1 - g->x0 + 1, buf, sizeof(buf));
	lcd_draw(l, f, g->cx - lcd_text_w(f, buf, 1) / 2, y, buf, c, LCD_TRANSPARENT, 1);
}

/* Largest of the given fonts in which s fits the content width; the last one truncates. */
static void text_center_best(lcd_t *l, const geom_t *g, const font_t **fs, int nfs, int y_top, int y_bottom,
			     const char *s, uint32_t c)
{
	int w = g->x1 - g->x0 + 1, i = 0;
	while (i < nfs - 1 && lcd_text_w(fs[i], s, 1) > w) i++;
	text_center(l, g, fs[i], y_top + (y_bottom - y_top - fs[i]->h) / 2, s, c);
}

static void row(lcd_t *l, const geom_t *g, int y, const char *label, const char *value, uint32_t vc)
{
	char buf[32];
	lcd_draw(l, &FONT_T16, g->x0, y, label, C_DIM, LCD_TRANSPARENT, 1);
	fit(&FONT_T16B, value, g->x1 + 1 - g->x0 - lcd_text_w(&FONT_T16, label, 1) - 8, buf, sizeof(buf));
	lcd_draw(l, &FONT_T16B, g->x1 + 1 - lcd_text_w(&FONT_T16B, buf, 1), y, buf, vc, LCD_TRANSPARENT, 1);
}

static void draw_pill(lcd_t *l, const geom_t *g, int y, const char *text, uint32_t dot)
{
	int tw = lcd_text_w(&FONT_T16B, text, 1), w = 20 + tw + 6, x = g->cx - w / 2;
	lcd_rrect(l, x, y, w, 22, 11, C_PILL);
	lcd_disc(l, x + 11, y + 11, 4, dot);
	lcd_draw(l, &FONT_T16B, x + 20, y + 3, text, C_WHITE, LCD_TRANSPARENT, 1);
}

static void draw_bars(lcd_t *l, const geom_t *g, int bars, int base_y)
{
	const int bw = 12, gap = 5, n = 5;
	int x = g->cx - (n * bw + (n - 1) * gap) / 2;
	uint32_t lit = bars >= 3 ? C_OK : bars == 2 ? C_WARN : C_BAD;
	for (int i = 0; i < n; i++) {
		int h = 8 + i * 6;
		lcd_rrect(l, x + i * (bw + gap), base_y - h, bw, h, 2, i < bars ? lit : C_UNLIT);
	}
}

/* A chunky network plug for wired / USB uplinks. */
static void draw_plug(lcd_t *l, const geom_t *g, int top_y, uint32_t c)
{
	int x = g->cx - 24;
	lcd_rrect(l, x, top_y, 48, 26, 4, c);
	for (int i = 0; i < 5; i++) lcd_fill(l, x + 8 + i * 7, top_y + 5, 4, 10, C_BG);
	lcd_rrect(l, g->cx - 10, top_y + 26, 20, 7, 2, c);
}

static void draw(lcd_t *l, const geom_t *g, const status_t *s, const char *state)
{
	char buf[64];
	int up = !strcmp(s->up_state, "up");
	int wifi = !strcmp(s->up_type, "wifi");
	static const font_t *big[] = { &FONT_T24B, &FONT_T16B };

	lcd_fill(l, 0, 0, l->w, l->h, C_BG);

	if (!s->valid) {
		draw_pill(l, g, 6, "NO DATA", C_BAD);
		lcd_flush(l);
		return;
	}

	/* status pill */
	if (!strcmp(state, "noUplink")) draw_pill(l, g, 6, "NO UPLINK", C_BAD);
	else if (!strcmp(state, "noInternet")) draw_pill(l, g, 6, "NO INTERNET", C_WARN);
	else draw_pill(l, g, 6, "ONLINE", C_OK);

	/* uplink block: y 30..120 */
	if (!up) {
		text_center(l, g, &FONT_T24B, 58, "NO UPLINK", C_BAD);
		text_center(l, g, &FONT_T16, 90, "Searching...", C_DIM);
	} else if (wifi) {
		int bars = s->has_signal ? signal_bars(s->signal) : 0;
		draw_bars(l, g, bars, 66);
		if (s->has_signal) {
			snprintf(buf, sizeof(buf), "%d dBm", s->signal);
			text_center(l, g, &FONT_T24B, 70, buf, bars >= 3 ? C_OK : bars == 2 ? C_WARN : C_BAD);
		}
		text_center_best(l, g, big, 2, 98, 122, s->up_name[0] ? s->up_name : "Wi-Fi", C_WHITE);
	} else {
		draw_plug(l, g, 32, C_OK);
		text_center(l, g, &FONT_T24B, 70, !strcmp(s->up_type, "usb") ? "USB WAN" : "ETHERNET", C_WHITE);
		if (s->speed > 0) snprintf(buf, sizeof(buf), "%d Mbps", s->speed);
		else copy(buf, sizeof(buf), s->up_detail);
		text_center_best(l, g, big, 2, 98, 122, buf, C_DIM);
	}

	lcd_fill(l, g->x0, 126, g->x1 - g->x0 + 1, 2, C_RULE);

	/* details */
	lcd_draw(l, &FONT_T16, g->x0, 131, "WAN IP", C_DIM, LCD_TRANSPARENT, 1);
	fit(&FONT_T16B, s->wan_ip[0] ? s->wan_ip : "none", g->x1 + 1 - g->x0, buf, sizeof(buf));
	lcd_draw(l, &FONT_T16B, g->x0, 147, buf, s->wan_ip[0] ? C_WHITE : C_BAD, LCD_TRANSPARENT, 1);

	if (!strcmp(s->vpn_state, "up")) row(l, g, 171, "VPN", s->vpn_name[0] ? s->vpn_name : "On", C_OK);
	else if (!strcmp(s->vpn_state, "down")) row(l, g, 171, "VPN", "Off", C_BAD);
	else row(l, g, 171, "VPN", "None", C_DIM);

	snprintf(buf, sizeof(buf), "%d", s->clients);
	row(l, g, 188, "Clients", buf, C_WHITE);
	char up_s[32];
	fmt_uptime(s->uptime, up_s, sizeof(up_s));
	row(l, g, 205, "Uptime", up_s, C_WHITE);

	lcd_flush(l);
}

int main(int argc, char **argv)
{
	const char *cmd = "/usr/libexec/utr-lcd/collect.sh";
	int interval = 5, rotate = 1, once = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc) cmd = argv[++i];
		else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-0")) rotate = 0;
		else if (!strcmp(argv[i], "-1")) once = 1;
		else { fprintf(stderr, "usage: %s [-c collector] [-i sec] [-0] [-1]\n", argv[0]); return 2; }
	}
	if (interval < 1) interval = 1;

	struct sigaction sa = { .sa_handler = on_sig };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	lcd_t l;
	if (lcd_open(&l, "/dev/dri/card0", rotate, rotate)) return 1;

	/*
	 * Visible window of the glass, measured with a calibration grid photographed straight on (see
	 * docs/decisions.md): in the mounted orientation the bezel hides ~5 px at the viewer's left,
	 * ~1 px at the right, and rounds the corners (~31 px radius, measured). Keep 6 px of margin beyond that.
	 */
	int clip_l = rotate ? 5 : 1, clip_r = rotate ? 1 : 5;
	geom_t g = { .x0 = clip_l + 8, .x1 = l.w - 1 - clip_r - 10 };
	g.cx = (g.x0 + g.x1) / 2;

	char sig[512] = "", prev[512] = "";
	int fails = 0;
	while (!stop) {
		status_t st;
		collect(cmd, &st);
		int up = !strcmp(st.up_state, "up");
		fails = (up && !strcmp(st.inet, "fail")) ? fails + 1 : 0;
		const char *state = !up ? "noUplink" : fails >= 2 ? "noInternet" : "ready";

		char up_s[32];
		fmt_uptime(st.uptime, up_s, sizeof(up_s));
		snprintf(sig, sizeof(sig), "%s|%d|%s|%s|%s|%s|%d|%d|%d|%s|%s|%s|%d|%s", state, st.valid, st.up_type, st.up_name,
			 st.up_detail, st.wan_ip, st.has_signal, st.signal, st.speed, st.vpn_state, st.vpn_name,
			 st.inet, st.clients, up_s);
		if (strcmp(sig, prev)) {
			draw(&l, &g, &st, state);
			strcpy(prev, sig);
		}
		if (once) { sleep(LCD_ONCE_HOLD); break; }
		for (int i = 0; i < interval && !stop; i++) sleep(1);
	}

	if (!once || LCD_ONCE_HOLD > 0) {      /* blank the panel on exit (not for zero-hold previews) */
		lcd_fill(&l, 0, 0, l.w, l.h, LCD_BLACK);
		lcd_flush(&l);
	}
	lcd_close(&l);
	return 0;
}
