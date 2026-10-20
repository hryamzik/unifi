/*
 * utr-lcd-status - render router status on the UTR-LR LCD.
 *
 * Runs a collector script (key=value lines, see collect.sh) every few seconds and redraws
 * the 136x240 panel only when the rendered text changes.
 *
 * Design for off-axis viewing: black background, large high-contrast text, no thin lines, every
 * state also spelled out in words (never colour alone), 8 px safe margin.
 *
 * Usage: utr-lcd-status [-c collector] [-i seconds] [-0] [-1]
 *   -c  collector command   (default /usr/libexec/utr-lcd/collect.sh)
 *   -i  refresh interval    (default 5)
 *   -0  do not rotate 180 degrees (default rotates; the panel is mounted upside-down)
 *   -1  draw once, hold 30 s, exit (for testing)
 */
#include <signal.h>
#include <stdarg.h>
#include "lcd.h"

/* Visible window measured from photos: ~12 px hidden on the left, ~4 px on the right; the bottom
 * edge disappears when viewed at an angle, so the layout ends well above y=240. */
#define MARGIN_L 12
#define MARGIN_R 4
#define MAXLINES 16

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

typedef struct {
	char up_state[16], up_type[16], up_name[64], up_detail[64], wan_ip[48];
	char vpn_state[16], vpn_name[48];
	int clients;
	long uptime;
	int valid;
} status_t;

typedef struct { int y, scale; uint32_t color; char text[40]; } line_t;

static const uint32_t C_LABEL = RGB(0, 220, 255);   /* cyan  - section labels */
static const uint32_t C_OK    = RGB(0, 255, 0);
static const uint32_t C_BAD   = RGB(255, 60, 60);
static const uint32_t C_WARN  = RGB(255, 220, 0);

static void copy(char *dst, size_t n, const char *src)
{
	snprintf(dst, n, "%s", src);
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
		else if (!strcmp(buf, "wan_ip")) copy(s->wan_ip, sizeof(s->wan_ip), eq);
		else if (!strcmp(buf, "vpn_state")) copy(s->vpn_state, sizeof(s->vpn_state), eq);
		else if (!strcmp(buf, "vpn_name")) copy(s->vpn_name, sizeof(s->vpn_name), eq);
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

static int add(line_t *ls, int n, int y, int scale, uint32_t color, const char *fmt, ...)
	__attribute__((format(printf, 6, 7)));
static int add(line_t *ls, int n, int y, int scale, uint32_t color, const char *fmt, ...)
{
	if (n >= MAXLINES) return n;
	va_list ap;
	va_start(ap, fmt);
	ls[n].y = y; ls[n].scale = scale; ls[n].color = color;
	vsnprintf(ls[n].text, sizeof(ls[n].text), fmt, ap);
	va_end(ap);
	return n + 1;
}

/* Turn a status into positioned text lines. */
static int layout(const status_t *s, line_t *ls)
{
	int n = 0, up = !strcmp(s->up_state, "up");
	char up_s[32];

	if (!s->valid) {
		n = add(ls, n, 90, 2, C_BAD, "NO DATA");
		return n;
	}

	n = add(ls, n, 8, 1, C_LABEL, "UPLINK");
	if (up) {
		const char *t = !strcmp(s->up_type, "wifi") ? "WIFI" : !strcmp(s->up_type, "eth") ? "ETH" : "LINK";
		n = add(ls, n, 24, 2, C_OK, "%s UP", t);
		n = add(ls, n, 58, 1, LCD_WHITE, "%s", s->up_name);
		n = add(ls, n, 74, 1, LCD_WHITE, "%s", s->up_detail);
	} else {
		n = add(ls, n, 24, 2, C_BAD, "DOWN");
		n = add(ls, n, 58, 1, C_BAD, "no default route");
	}

	n = add(ls, n, 94, 1, C_LABEL, "WAN IP");
	n = add(ls, n, 110, 1, s->wan_ip[0] ? LCD_WHITE : C_BAD, "%s", s->wan_ip[0] ? s->wan_ip : "none");

	n = add(ls, n, 130, 1, C_LABEL, "VPN");
	if (!strcmp(s->vpn_state, "up"))
		n = add(ls, n, 146, 1, C_OK, "ON  %s", s->vpn_name);
	else if (!strcmp(s->vpn_state, "down"))
		n = add(ls, n, 146, 1, C_BAD, "OFF %s", s->vpn_name);
	else
		n = add(ls, n, 146, 1, C_WARN, "not configured");

	n = add(ls, n, 166, 1, C_LABEL, "CLIENTS");
	n = add(ls, n, 166, 1, LCD_WHITE, "        %d", s->clients); /* value, offset past the label */

	n = add(ls, n, 186, 1, C_LABEL, "UPTIME");
	fmt_uptime(s->uptime, up_s, sizeof(up_s));
	n = add(ls, n, 202, 1, LCD_WHITE, "%s", up_s);
	return n;
}

static void draw(lcd_t *l, const line_t *ls, int n)
{
	lcd_fill(l, 0, 0, l->w, l->h, LCD_BLACK);
	for (int i = 0; i < n; i++) {
		char buf[40];
		int maxc = (l->w - MARGIN_L - MARGIN_R) / (FONT_W * ls[i].scale);
		/* a line may start with spaces to offset a value past a label; treat as transparent */
		const char *t = ls[i].text;
		int x = MARGIN_L;
		while (*t == ' ') { x += FONT_W * ls[i].scale; t++; maxc--; }
		snprintf(buf, sizeof(buf), "%.*s", maxc > 0 ? maxc : 0, t);
		lcd_text(l, x, ls[i].y, buf, ls[i].color, LCD_BLACK, ls[i].scale);
	}
	lcd_flush(l);
}

static void signature(const line_t *ls, int n, char *sig, size_t sz)
{
	sig[0] = 0;
	for (int i = 0; i < n; i++) {
		size_t used = strlen(sig);
		snprintf(sig + used, sz - used, "%d,%d,%x,%s|", ls[i].y, ls[i].scale, ls[i].color, ls[i].text);
	}
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

	char sig[1024] = "", prev[1024] = "";
	while (!stop) {
		status_t st;
		line_t ls[MAXLINES];
		collect(cmd, &st);
		int n = layout(&st, ls);
		signature(ls, n, sig, sizeof(sig));
		if (strcmp(sig, prev)) {
			draw(&l, ls, n);
			strcpy(prev, sig);
		}
		if (once) { sleep(30); break; }
		for (int i = 0; i < interval && !stop; i++) sleep(1);
	}

	lcd_fill(&l, 0, 0, l.w, l.h, LCD_BLACK);
	lcd_flush(&l);
	lcd_close(&l);
	return 0;
}
