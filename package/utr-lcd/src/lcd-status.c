/*
 * utr-lcd-status - render router status on the UTR-LR LCD (built-in renderer).
 *
 * A screen is a vertical stack of widgets (SSID, signal, clients, IPs, VPN, uptime, ...) drawn over
 * an optional Wi-Fi signal backfill; the online status is a widget like any other. Several screens can be
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
 * Usage: utr-lcd-status [-c collector] [-C screens.conf] [-T traffic] [-i seconds] [-0] [-1 [-N screen]] [-P] [-l]
 *   -c  collector command   (default /usr/libexec/utr-lcd/collect.sh)
 *   -C  screen layout file  (default /var/run/utr-lcd/screens.conf)
 *   -T  traffic file        (default /var/run/utr-lcd/traffic, written by traffic.sh)
 *   -i  refresh interval    (default 5)
 *   -0  do not rotate 180 degrees (default rotates; the panel is mounted upside-down)
 *   -1  draw once, hold, exit (for testing; hold time is LCD_ONCE_HOLD seconds)
 *   -N  with -1: which screen to draw (1-based, default 1)
 *   -P  headless preview: like -1 but nothing is shown; prints the screen as run-length text on stdout (LuCI)
 *   -l  list the configured screens (number, enabled, dwell, name) and exit
 * Env: LCD_TRACE=1 prints a line (ms, screen number) to stderr whenever a screen is drawn.
 */
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
	int nups;                                  /* uplinks, lowest metric first (ups[0] is the one in up_*) */
	struct { char dev[16], type[16], name[64]; int signal, has_signal, speed, metric; } ups[3];
	int nmw;                                   /* mwan3 interfaces (from ubus status via collect.sh), if any */
	struct { char ifname[16], dev[16], state[12]; int pct; } mw[3];   /* pct: share in the default policy, -1 = unknown */
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
	else if (!strcmp(line, "ups")) s->nups = atoi(eq) > 3 ? 3 : atoi(eq);
	else if (!strcmp(line, "mws")) s->nmw = atoi(eq) > 3 ? 3 : atoi(eq);
	else if (!strncmp(line, "mw", 2) && line[2] >= '0' && line[2] <= '2' && line[3] == '_') {
		int n = line[2] - '0';
		const char *k = line + 4;
		if (!strcmp(k, "if")) copy(s->mw[n].ifname, sizeof(s->mw[n].ifname), eq);
		else if (!strcmp(k, "dev")) copy(s->mw[n].dev, sizeof(s->mw[n].dev), eq);
		else if (!strcmp(k, "state")) copy(s->mw[n].state, sizeof(s->mw[n].state), eq);
		else if (!strcmp(k, "pct")) s->mw[n].pct = atoi(eq);
		else return;
	}
	else if (!strncmp(line, "up", 2) && line[2] >= '0' && line[2] <= '2' && line[3] == '_') {
		int n = line[2] - '0';
		const char *k = line + 4;
		if (!strcmp(k, "dev")) copy(s->ups[n].dev, sizeof(s->ups[n].dev), eq);
		else if (!strcmp(k, "type")) copy(s->ups[n].type, sizeof(s->ups[n].type), eq);
		else if (!strcmp(k, "name")) copy(s->ups[n].name, sizeof(s->ups[n].name), eq);
		else if (!strcmp(k, "signal")) { s->ups[n].has_signal = *eq != 0; s->ups[n].signal = atoi(eq); }
		else if (!strcmp(k, "speed")) s->ups[n].speed = atoi(eq);
		else if (!strcmp(k, "metric")) s->ups[n].metric = atoi(eq);
		else return;
	}
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
	for (int i = 0; i < 3; i++) c->next.mw[i].pct = -1;
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

/* ------------------------------------------------------------------------------------- traffic */

#define MAX_CL 24
#define TRAFFIC_COLORS 12
typedef struct { char id[24], name[32]; double rate, total; double drate, urate, dtot, utot; int online, color; int signal, has_signal; } cl_t;
typedef struct { char dev[16]; double down, up, dtot, utot; } upl_t;
typedef struct { int valid; double down, up, dtot, utot; cl_t c[MAX_CL]; int n; upl_t u[3]; int nu; } traffic_t;

static const uint32_t PALETTE[TRAFFIC_COLORS] = {
	RGB(0, 200, 255), RGB(255, 150, 30), RGB(230, 70, 200), RGB(150, 230, 40), RGB(150, 110, 255), RGB(255, 220, 50),
	RGB(40, 210, 170), RGB(255, 100, 100), RGB(60, 120, 255), RGB(255, 150, 200), RGB(120, 255, 190), RGB(200, 160, 80) };
#define C_OTHER RGB(120, 130, 150)
#define C_TRACK RGB(40, 46, 64)
#define C_BAR_NODATA RGB(235, 238, 245)   /* a bar with no data in it: white, not a client colour */
#define C_DOWN  RGB(0, 200, 255)
#define C_UP    RGB(255, 150, 30)

/* Stable colour per client: from a hash of its id, probing on collisions (assigned in hash order). */
static void traffic_colors(traffic_t *t)
{
	unsigned h[MAX_CL];
	int order[MAX_CL], used[TRAFFIC_COLORS] = { 0 };
	for (int i = 0; i < t->n; i++) {
		unsigned x = 2166136261u;
		for (const char *p = t->c[i].id; *p; p++) x = (x ^ (unsigned char)*p) * 16777619u;
		h[i] = x;
		order[i] = i;
	}
	for (int i = 1; i < t->n; i++) {                     /* insertion sort by hash */
		int v = order[i], j = i - 1;
		while (j >= 0 && h[order[j]] > h[v]) { order[j + 1] = order[j]; j--; }
		order[j + 1] = v;
	}
	for (int k = 0; k < t->n; k++) {
		int i = order[k], idx = (int)(h[i] % TRAFFIC_COLORS);
		for (int tries = 0; tries < TRAFFIC_COLORS && used[idx]; tries++) idx = (idx + 1) % TRAFFIC_COLORS;
		used[idx] = 1;
		t->c[i].color = idx;
	}
}

/* Read the sampler's file (see traffic.sh); unusable when missing or older than 15 s. */
static void traffic_load(traffic_t *t, const char *path)
{
	memset(t, 0, sizeof(*t));
	struct stat sb;
	if (stat(path, &sb) || (!getenv("LCD_TRAFFIC_ANY_AGE") && time(NULL) - sb.st_mtime > 15)) return;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[160];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq++ = 0;
		if (!strcmp(line, "down_bps")) { t->down = atof(eq); t->valid = 1; }
		else if (!strcmp(line, "up_bps")) t->up = atof(eq);
		else if (!strcmp(line, "down_total")) t->dtot = atof(eq);
		else if (!strcmp(line, "up_total")) t->utot = atof(eq);
		else if (!strcmp(line, "uplink") && t->nu < 3) {
			char *f1 = strtok(eq, "|"), *f2 = strtok(NULL, "|"), *f3 = strtok(NULL, "|"), *f4 = strtok(NULL, "|"), *f5 = strtok(NULL, "|");
			if (!f1 || !f2 || !f3) continue;
			copy(t->u[t->nu].dev, sizeof(t->u[t->nu].dev), f1);
			t->u[t->nu].down = atof(f2); t->u[t->nu].up = atof(f3);
			if (f4) t->u[t->nu].dtot = atof(f4);
			if (f5) t->u[t->nu].utot = atof(f5);
			t->nu++;
		}
		else if (!strcmp(line, "client") && t->n < MAX_CL) {
			cl_t *c = &t->c[t->n];
			char *fld[10] = { 0 };
			int nf = 0;
			for (char *cur = eq; nf < 10; ) {          /* split on '|', keeping empty fields */
				fld[nf++] = cur;
				char *sep = strchr(cur, '|');
				if (!sep) break;
				*sep = 0; cur = sep + 1;
			}
			if (nf < 5) continue;
			copy(c->id, sizeof(c->id), fld[0]);
			copy(c->name, sizeof(c->name), fld[1]);
			c->rate = atof(fld[2]); c->total = atof(fld[3]); c->online = atoi(fld[4]);
			/* optional: signal dBm (empty = none), down/up bps, down/up bytes */
			if (nf > 5 && *fld[5]) { c->has_signal = 1; c->signal = atoi(fld[5]); }
			if (nf > 6) c->drate = atof(fld[6]);
			if (nf > 7) c->urate = atof(fld[7]);
			if (nf > 8) c->dtot = atof(fld[8]);
			if (nf > 9) c->utot = atof(fld[9]);
			t->n++;
		}
	}
	fclose(f);
	traffic_colors(t);
}

/* Bar/legend metrics: combined or one direction, rates or byte totals. */
enum { BM_RATE, BM_TOTAL, BM_DRATE, BM_URATE, BM_DTOTAL, BM_UTOTAL };
static double client_val(const cl_t *c, int mode)
{
	switch (mode) {
	case BM_DRATE: return c->drate;
	case BM_URATE: return c->urate;
	case BM_DTOTAL: return c->dtot;
	case BM_UTOTAL: return c->utot;
	case BM_TOTAL: return c->total;
	default: return c->rate;
	}
}

/* Indices of the clients by metric, largest first; only those with something to show. */
static int traffic_rank_m(const traffic_t *t, int mode, int *idx)
{
	int by_total = mode == BM_TOTAL || mode == BM_DTOTAL || mode == BM_UTOTAL;
	int n = 0;
	for (int i = 0; i < t->n; i++) {
		double v = client_val(&t->c[i], mode);
		if (v <= 0 && !(by_total ? 0 : t->c[i].online)) continue;
		int j = n++;
		while (j > 0) {
			const cl_t *a = &t->c[idx[j - 1]], *b = &t->c[i];
			double av = client_val(a, mode), bv = client_val(b, mode);
			if (av > bv || (av == bv && a->total >= b->total)) break;
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = i;
	}
	return n;
}
static int traffic_rank(const traffic_t *t, int by_total, int *idx) { return traffic_rank_m(t, by_total ? BM_TOTAL : BM_RATE, idx); }

/* Speed as a number and a unit, e.g. 12.5 + "Mb/s". */
static void fmt_rate(double bps, char *num, size_t n, const char **unit)
{
	double v = bps;
	*unit = "b/s";
	if (bps >= 1e9) { v = bps / 1e9; *unit = "Gb/s"; }
	else if (bps >= 1e6) { v = bps / 1e6; *unit = "Mb/s"; }
	else if (bps >= 1e3) { v = bps / 1e3; *unit = "kb/s"; }
	if (v < 10 && *unit[0] != 'b') snprintf(num, n, "%.1f", v);
	else snprintf(num, n, "%.0f", v);
}

/* At most 4 characters: 340, 1.2k, 12M, 3.4G. */
static void fmt_short(double v, char *out, size_t n)
{
	static const char U[] = " kMGT";
	int u = 0;
	while (v >= 999.5 && u < 4) { v /= 1000; u++; }
	if (u && v < 9.95) snprintf(out, n, "%.1f%c", v, U[u]);
	else snprintf(out, n, "%.0f%c", v, u ? U[u] : '\0');
}

static void fmt_bytes(double v, char *out, size_t n)
{
	static const char *U[] = { "B", "kB", "MB", "GB", "TB" };
	int u = 0;
	while (v >= 999.5 && u < 4) { v /= 1000; u++; }
	if (u && v < 99.95) snprintf(out, n, "%.1f %s", v, U[u]);
	else snprintf(out, n, "%.0f %s", v, U[u]);
}

/* ------------------------------------------------------------------------------------- resources */

typedef struct { int valid, cpu, ram, flash, temp; int load[3]; int core[8], ncores; } res_t;     /* percent; temp C, -1 = none; load in hundredths */
typedef struct { unsigned long long total, idle; int have; unsigned long long ctot[8], cidle[8]; int chave[8]; } cpu_prev_t;

static void res_sample(res_t *r, cpu_prev_t *cp)
{
	FILE *f = fopen("/proc/stat", "r");
	if (f) {
		char line[256];
		r->ncores = 0;
		while (fgets(line, sizeof(line), f)) {
			unsigned long long v[8] = { 0 };
			int agg = !strncmp(line, "cpu ", 4), core = -1;
			if (!agg && sscanf(line, "cpu%d", &core) != 1) continue;
			const char *p = strchr(line, ' ');
			if (!p || sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4) continue;
			unsigned long long tot = 0, idle = v[3] + v[4];
			for (int i = 0; i < 8; i++) tot += v[i];
			if (agg) {
				if (cp->have && tot > cp->total) r->cpu = (int)(100 - (idle - cp->idle) * 100 / (tot - cp->total));
				cp->total = tot; cp->idle = idle; cp->have = 1;
			} else if (core >= 0 && core < 8) {
				if (core + 1 > r->ncores) r->ncores = core + 1;
				if (cp->chave[core] && tot > cp->ctot[core]) r->core[core] = (int)(100 - (idle - cp->cidle[core]) * 100 / (tot - cp->ctot[core]));
				if (r->core[core] < 0) r->core[core] = 0;
				cp->ctot[core] = tot; cp->cidle[core] = idle; cp->chave[core] = 1;
			}
		}
		fclose(f);
	}
	f = fopen("/proc/loadavg", "r");
	if (f) {
		double l1, l5, l15;
		if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
			r->load[0] = (int)(l1 * 100 + 0.5); r->load[1] = (int)(l5 * 100 + 0.5); r->load[2] = (int)(l15 * 100 + 0.5);
		}
		fclose(f);
	}
	f = fopen("/proc/meminfo", "r");
	if (f) {
		char line[96];
		unsigned long long tot = 0, avail = 0, fr = 0, buf = 0, cache = 0, x;
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "MemTotal: %llu", &x) == 1) tot = x;
			else if (sscanf(line, "MemAvailable: %llu", &x) == 1) avail = x;
			else if (sscanf(line, "MemFree: %llu", &x) == 1) fr = x;
			else if (sscanf(line, "Buffers: %llu", &x) == 1) buf = x;
			else if (sscanf(line, "Cached: %llu", &x) == 1) cache = x;
		}
		fclose(f);
		if (!avail) avail = fr + buf + cache;
		if (tot) r->ram = (int)(100 - avail * 100 / tot);
	}
	struct statvfs sv;
	if (!statvfs("/overlay", &sv) || !statvfs("/", &sv))
		if (sv.f_blocks) r->flash = (int)(100 - (unsigned long long)sv.f_bavail * 100 / sv.f_blocks);
	r->temp = -1;
	for (int i = 0; i < 8 && r->temp < 0; i++) {
		char path[64];
		int mc;
		snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp1_input", i);
		f = fopen(path, "r");
		if (f) { if (fscanf(f, "%d", &mc) == 1) r->temp = mc / 1000; fclose(f); }
	}
	if (r->cpu < 0) r->cpu = 0;
	r->valid = 1;
}

/* ---------------------------------------------------------------------------------- configuration */

enum { W_SSID, W_DBM, W_BARS, W_SIGBAR, W_FAN, W_SEGBAR, W_STATUS, W_CLIENTS, W_WANIP, W_LANIP, W_VPN, W_UPTIME, W_RULE,
       W_SPEED, W_SPEED1, W_SPEEDBAR, W_SPEEDDN, W_SPEEDUP, W_USAGE, W_USAGEBAR, W_USAGEDN, W_USAGEUP, W_LEGEND, W_LEGENDTOTAL, W_DONUT,
       W_CPU, W_RAM, W_FLASH, W_LOAD, W_TEMP, W_CPURAM, W_MULTIWAN, W_UPLINKS, W_ROW, W_NTYPES };
enum { ST_DOT, ST_PILL, ST_NONE };   /* legacy "indicator" config values, parsed but no longer drawn */

typedef struct { const char *name; int def_size; } wtype_t;
static const wtype_t WTYPES[W_NTYPES] = {
	[W_SSID]    = { "ssid", 24 },
	[W_DBM]     = { "dbm", 32 },
	[W_BARS]    = { "bars", 24 },           /* Wi-Fi signal as 5 ascending bars (Wi-Fi only); size = tallest bar */
	[W_SIGBAR]  = { "sigbar", 16 },         /* Wi-Fi signal as a resource-style line bar; size >= 24: tall bar, text on top */
	[W_FAN]     = { "fan", 32 },            /* Wi-Fi signal as a fan icon (dot + arcs), Wi-Fi only */
	[W_SEGBAR]  = { "segbar", 16 },         /* Wi-Fi signal as the stock-style segmented bar (10 segments), Wi-Fi only */
	[W_STATUS]  = { "status", 16 },         /* online status globe (was "dot" in early configs) */
	[W_ROW]     = { "row", 0 },             /* sub-widgets side by side on one line (a+b+c in the config) */
	[W_CLIENTS] = { "clients", 24 },
	[W_WANIP]   = { "wanip", 16 },
	[W_LANIP]   = { "lanip", 16 },
	[W_VPN]     = { "vpn", 16 },
	[W_UPTIME]  = { "uptime", 16 },
	[W_RULE]    = { "rule", 2 },
	[W_SPEED]   = { "speed", 24 },          /* down and up speed, two lines; size 16 or 24 */
	[W_SPEED1]  = { "speed1", 16 },         /* down and up speed on one line */
	[W_SPEEDBAR] = { "speedbar", 12 },      /* current speed split by client, size = bar height 8/12/16 */
	[W_SPEEDDN] = { "speeddn", 12 },        /* download speed split by client */
	[W_SPEEDUP] = { "speedup", 12 },        /* upload speed split by client */
	[W_USAGEDN] = { "usagedn", 16 },        /* downloaded bytes split by client, total inside the bar (16/24) */
	[W_USAGEUP] = { "usageup", 16 },        /* uploaded bytes split by client, total inside the bar (16/24) */
	[W_USAGE]   = { "usage", 16 },          /* total traffic since the sampler started */
	[W_USAGEBAR] = { "usagebar", 12 },      /* total traffic split by client */
	[W_LEGEND]  = { "legend", 4 },          /* size = number of client rows; colour, name, current speed */
	[W_LEGENDTOTAL] = { "legendtotal", 4 }, /* same, with each client's total */
	[W_DONUT]   = { "donut", 80 },          /* current speed split by client, size = diameter */
	[W_CPU]     = { "cpu", 16 },            /* resource rows: label, bar, percent; size 12 or 16 */
	[W_RAM]     = { "ram", 16 },
	[W_FLASH]   = { "flash", 16 },
	[W_LOAD]    = { "load", 16 },           /* load average; bar scaled to the core count */
	[W_MULTIWAN] = { "multiwan", 16 },      /* share of traffic per uplink (hidden with a single uplink); size 12 or 16 */
	[W_UPLINKS] = { "uplinks", 24 },        /* uplink icons + state (mwan3 when available); never hidden; size = icon 16 or 24 */
	[W_TEMP]    = { "temp", 16 },           /* Wi-Fi chip temperature (hidden when there is no sensor) */
	[W_CPURAM]  = { "cpuram", 12 },         /* CPU and RAM side by side on one line (for non-resource screens) */
};

#define MAX_WIDGETS 16
#define MAX_SCREENS 12
#define MAX_PLAYLIST 32

#define MAX_SUB 3
typedef struct { int type, size, wrap, shrink, bg, fit, cores, layout, generic, noname, noicon, showval, peak, split; } wspec_t;   /* bg/fit/peak/split: legend; cores: cpu; layout/generic/noname/noicon: uplinks */
typedef struct { int type, size, wrap, shrink, bg, fit, cores, layout, generic, noname, noicon, showval, peak, split; wspec_t sub[MAX_SUB]; int nsub; } widget_t;   /* nsub > 0: type is W_ROW */
typedef struct {
	char name[24];
	int dwell_ms, enabled, indicator, backfill, debug;
	int spacing;                       /* gap between widgets in px; -1 = auto (spread, capped) */
	widget_t w[MAX_WIDGETS];
	int nw;
} screen_t;
typedef struct {
	screen_t s[MAX_SCREENS];
	int ns;
	int pinned, pin;                   /* mode=pin: show only screen `pin` (1-based) */
	int playlist[MAX_PLAYLIST], npl;   /* 1-based screen numbers; empty = every enabled screen in order */
	int fill_dim, fill_edge;           /* signal backfill: brightness percent, bright edge line */
	int name_len;                      /* client names are cut to this many characters */
} config_t;

static const char DEFAULT_CONF[] =
	"fill_dim=30\nfill_edge=1\n"
	"screen=Status\ndwell=8\nbackfill=0\nwidget=status:16\nwidget=segbar:16\nwidget=dbm:32\nwidget=ssid:24ws\n"
	"widget=clients:24\n"
	"screen=Traffic\ndwell=8\nbackfill=1\nwidget=speed:24\nwidget=speedbar:12\nwidget=usage:16\n"
	"widget=usagebar:12\nwidget=legend:4\n"
	"screen=Details\ndwell=5\nbackfill=1\n"
	"widget=wanip:16\nwidget=lanip:16\nwidget=vpn:16\nwidget=uptime:16\n"
	"widget=cpu:16\nwidget=ram:16\nwidget=flash:16\nwidget=load:16\nwidget=temp:16\n";

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
		s->dwell_ms = 8000; s->enabled = 1; s->indicator = ST_DOT; s->backfill = 1; s->spacing = -1;
	} else if (!s) {
		if (!strcmp(line, "mode")) c->pinned = !strcmp(eq, "pin");
		else if (!strcmp(line, "pin")) c->pin = atoi(eq);
		else if (!strcmp(line, "fill_dim")) c->fill_dim = atoi(eq);
		else if (!strcmp(line, "fill_edge")) c->fill_edge = atoi(eq);
		else if (!strcmp(line, "name_len")) c->name_len = atoi(eq);
		else if (!strcmp(line, "playlist")) {
			for (char *t = strtok(eq, " ,"); t && c->npl < MAX_PLAYLIST; t = strtok(NULL, " ,"))
				if (atoi(t) > 0) c->playlist[c->npl++] = atoi(t);
		}
	} else if (!strcmp(line, "dwell")) {
		int ms = (int)(atof(eq) * 1000);
		s->dwell_ms = ms < 500 ? 500 : ms > 3600000 ? 3600000 : ms;       /* 0.5 s .. 1 h */
	} else if (!strcmp(line, "enabled")) s->enabled = atoi(eq) != 0;
	else if (!strcmp(line, "spacing")) {
		s->spacing = !strcmp(eq, "auto") ? -1 : atoi(eq);
		if (s->spacing > 30) s->spacing = 30;
	}
	else if (!strcmp(line, "indicator")) s->indicator = !strcmp(eq, "pill") ? ST_PILL : !strcmp(eq, "none") ? ST_NONE : ST_DOT;
	else if (!strcmp(line, "backfill")) s->backfill = atoi(eq) != 0;
	else if (!strcmp(line, "debug")) s->debug = atoi(eq) != 0;
	else if (!strcmp(line, "widget") && s->nw < MAX_WIDGETS) {
		/* one widget, or several joined by '+' to share one line (at most MAX_SUB) */
		wspec_t specs[MAX_SUB + 1];
		int n = 0;
		for (char *tok = strtok(eq, "+"); tok && n <= MAX_SUB; tok = strtok(NULL, "+")) {
			char *colon = strchr(tok, ':');
			int size = 0;
			if (colon) { *colon++ = 0; size = atoi(colon); }
			if (!strcmp(tok, "dot")) tok = "status";      /* name before the globe icon replaced the dot */
			for (int t = 0; t < W_NTYPES; t++)
				if (!strcmp(tok, WTYPES[t].name) && t != W_ROW) {
					specs[n].type = t;
					specs[n].size = size > 0 ? size : WTYPES[t].def_size;
					specs[n].wrap = colon && strchr(colon, 'w') ? 1 : 0;   /* e.g. ssid:24w wraps to 2 lines */
					specs[n].shrink = colon && strchr(colon, 's') ? 1 : 0;   /* ssid:24ws: then step the font down until it fits */
					specs[n].cores = colon && strchr(colon, 'r') ? 2 : colon && strchr(colon, 'c') ? 1 : 0;   /* cpu:16c per-core segments, cpu:16r per-core rows */
					specs[n].layout = colon && strchr(colon, 'l') ? 1 : 0;   /* uplinks:24l: icon beside the label, not above */
					specs[n].generic = colon && strchr(colon, 'g') ? 1 : 0;   /* uplinks:24g: label is WiFi/Eth, not the network name */
					specs[n].noname = colon && strchr(colon, 'n') ? 1 : 0;   /* uplinks:24n: no labels, icons only */
					specs[n].noicon = colon && strchr(colon, 'i') ? 1 : 0;   /* uplinks:24i: no icons, labels only */
					specs[n].showval = colon && strchr(colon, 'v') ? 1 : 0;   /* speeddn:12v: total speed written on the bar */
					specs[n].peak = colon && strchr(colon, 'p') ? 1 : 0;   /* legend:4b4p: bar scaled by the peak rate seen, not the current max */
					specs[n].split = colon && strchr(colon, 'x') ? 1 : 0;   /* legend:4b4x: bar split into upload (thin, top) and download */
					specs[n].fit = colon && strchr(colon, 'F') ? 2 : colon && strchr(colon, 'f') ? 1 : 0;   /* legend fit: f gentle, F compact */
					if (colon) {   /* legend background: b0..b5 (none speed total signal signal+speed signal+total) */
						const char *b = strchr(colon, 'b');
						if (b && b[1] >= '0' && b[1] <= '5') specs[n].bg = b[1] - '0';
					}
					n++;
					break;
				}
		}
		if (n == 1) {
			s->w[s->nw].type = specs[0].type;
			s->w[s->nw].size = specs[0].size;
			s->w[s->nw].wrap = specs[0].wrap;
			s->w[s->nw].shrink = specs[0].shrink;
			s->w[s->nw].bg = specs[0].bg;
			s->w[s->nw].fit = specs[0].fit;
			s->w[s->nw].cores = specs[0].cores;
			s->w[s->nw].layout = specs[0].layout;
			s->w[s->nw].generic = specs[0].generic;
			s->w[s->nw].noname = specs[0].noname;
			s->w[s->nw].noicon = specs[0].noicon;
			s->w[s->nw].showval = specs[0].showval;
			s->w[s->nw].peak = specs[0].peak;
			s->w[s->nw].split = specs[0].split;
			s->nw++;
		} else if (n > 1) {
			widget_t *wd = &s->w[s->nw++];
			wd->type = W_ROW;
			wd->nsub = n;
			for (int i = 0; i < n; i++) wd->sub[i] = specs[i];
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
	c->fill_dim = 30; c->fill_edge = 1; c->name_len = 8;
	char text[4096];
	FILE *f = path ? fopen(path, "r") : NULL;
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	if (f) fclose(f);
	text[n] = 0;
	if (n) conf_parse(c, text);
	if (!c->ns) {                    /* missing or empty: built-in screens */
		int fd = c->fill_dim, fe = c->fill_edge, nl = c->name_len;
		memset(c, 0, sizeof(*c));
		c->fill_dim = fd; c->fill_edge = fe; c->name_len = nl;
		conf_parse(c, DEFAULT_CONF);
	}
	if (c->fill_dim < 5) c->fill_dim = 5;
	if (c->fill_dim > 100) c->fill_dim = 100;
	if (c->name_len < 3) c->name_len = 3;
	if (c->name_len > 16) c->name_len = 16;
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
	(void)w;
	char buf[64];
	int cx = (sp.x0 + sp.x1 + 1) / 2;          /* centred in the span (a cell of a shared line, or the screen) */
	int half = sp.x1 - cx < cx - sp.x0 ? sp.x1 - cx : cx - sp.x0, maxw = 2 * half + 1;
	const font_t *f = fit_font(ladder_index(size), s, maxw);
	ellipsize(f, s, maxw, middle, buf, sizeof(buf));
	lcd_draw(l, f, cx - lcd_text_w(f, buf, 1) / 2, y + (h - f->h) / 2, buf, c, LCD_TRANSPARENT, 1);
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
	if (label[0] && lw + 4 + lcd_text_w(VAL[st], value, 1) > width) {
		/* a narrow shared-line cell cannot carry both: the value alone, ellipsized */
		ellipsize(VAL[st], value, width - lw0, middle, buf, sizeof(buf));
		if (icon) draw_icon(l, sp.x0, y + (h - 16) / 2, icon, icon_c, 1);
		lcd_draw(l, VAL[st], sp.x1 + 1 - lcd_text_w(VAL[st], buf, 1), y + (h - VAL[st]->h) / 2, buf, vc, LCD_TRANSPARENT, 1);
		return;
	}
	ellipsize(VAL[st], value, width - lw - 4, middle, buf, sizeof(buf));
	if (icon) draw_icon(l, sp.x0, y + (h - 16) / 2, icon, icon_c, 1);
	lcd_draw(l, LAB[st], sp.x0 + lw0, y + (h - LAB[st]->h) / 2, label, C_DIM, LCD_TRANSPARENT, 1);
	lcd_draw(l, VAL[st], sp.x1 + 1 - lcd_text_w(VAL[st], buf, 1), y + (h - VAL[st]->h) / 2, buf, vc, LCD_TRANSPARENT, 1);
}

static void fmt_uptime_short(long t, char *out, size_t n)
{
	long d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60;
	if (d > 0) snprintf(out, n, "%ldd%ldh", d, h);
	else if (h > 0) snprintf(out, n, "%ldh%02ldm", h, m);
	else snprintf(out, n, "%ldm", m);
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
static void draw_backfill(lcd_t *l, int pct, int dim, int edge, int x0, int x1)
{
	int r, g, b = 0;
	if (pct < 50) { r = 255; g = pct * 51 / 10; } else { r = 255 - (pct - 50) * 51 / 10; g = 235; }
	int top = l->h - l->h * pct / 100;
	lcd_fill(l, x0, top, x1 - x0, l->h - top, scale_rgb(r, g, b, dim));
	if (edge && pct > 0 && pct < 100) lcd_fill(l, x0, top, x1 - x0, 3, scale_rgb(r, g, b, dim + 22 > 100 ? 100 : dim + 22));
}

static void draw_pill(lcd_t *l, const win_t *w, int y, const char *text, uint32_t dot)
{
	int tw = lcd_text_w(&FONT_T16B, text, 1), pw = 20 + tw + 6, x = w->cx - pw / 2;
	lcd_rrect(l, x, y, pw, 22, 11, C_PILL);
	lcd_disc(l, x + 11, y + 11, 4, dot);
	lcd_draw(l, &FONT_T16B, x + 20, y + 3, text, C_WHITE, LCD_TRANSPARENT, 1);
}

/* Down (1) or up (0) arrow, 12x10. */
static void draw_tri(lcd_t *l, int x, int y, int down, uint32_t c)
{
	for (int j = 0; j < 10; j++) {
		int w = down ? 12 - j * 12 / 10 : 2 + j * 12 / 10;
		if (w < 1) w = 1;
		lcd_fill(l, x + (12 - w) / 2, y + j, w, 1, c);
	}
}

#define BAR_MAX 6

/* Share bar: one segment per client in its colour (largest BAR_MAX, the rest grey), no text on it. */
/* Bar version of a client colour: lifted to a minimum luminance, same hue. On the real panel
 * the raw dark blues/purples make the dark bar text unreadable. */
static uint32_t bar_color(uint32_t c)
{
	int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255, lum = (r * 299 + g * 587 + b * 114) / 1000;
	if (lum <= 0 || lum >= 185) return c;
	r = r * 185 / lum; g = g * 185 / lum; b = b * 185 / lum;
	return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

/* Text over a bar whose fill may end mid-string: a 1 px shadow in the opposite tone keeps it
 * readable on the bright segment and on the dark track/background alike. */
static void draw_halo(lcd_t *l, const font_t *f, int x, int y, const char *s, uint32_t col)
{
	uint32_t sh = col == RGB(10, 12, 16) ? RGB(235, 238, 245) : RGB(10, 12, 16);
	lcd_draw(l, f, x + 1, y + 1, s, sh, LCD_TRANSPARENT, 1);
	lcd_draw(l, f, x, y, s, col, LCD_TRANSPARENT, 1);
}

static void draw_share_bar_m(lcd_t *l, const traffic_t *t, int mode, int x, int y, int w, int h, int dir, int showval)
{
	int idx[MAX_CL], n = traffic_rank_m(t, mode, idx), k = 0;
	double vals[BAR_MAX + 2], tot = 0, others = 0;
	uint32_t cols[BAR_MAX + 2];
	for (int i = 0; i < n; i++) {
		double v = client_val(&t->c[idx[i]], mode);
		if (v <= 0) continue;
		if (k < BAR_MAX) { vals[k] = v; cols[k] = bar_color(PALETTE[t->c[idx[i]].color]); k++; }
		else others += v;
		tot += v;
	}
	if (others > 0) { vals[k] = others; cols[k] = bar_color(C_OTHER); k++; }
	/* What no client accounts for - the router itself, missed stations - is the gap between the
	 * interface total and the sum of the clients, drawn white (beyond 2%: counter read jitter).
	 * With it, the bar is scaled by the real total instead of renormalising the clients. */
	double overall = mode == BM_DRATE ? t->down : mode == BM_URATE ? t->up :
		mode == BM_DTOTAL ? t->dtot : mode == BM_UTOTAL ? t->utot :
		mode == BM_TOTAL ? t->dtot + t->utot : t->down + t->up;
	if (overall > 0 && overall - tot > overall * 0.02) { vals[k] = overall - tot; cols[k] = C_BAR_NODATA; k++; tot = overall; }
	lcd_rrect(l, x, y, w, h, 3, k ? C_TRACK : C_BAR_NODATA);
	double acc = 0;
	for (int i = 0; i < k && tot > 0; i++) {
		int x0 = x + (int)(acc / tot * w + 0.5);
		acc += vals[i];
		int x1 = x + (int)(acc / tot * w + 0.5);
		if (x1 - x0 > 1) lcd_fill(l, x0 + (i ? 1 : 0), y + 1, x1 - x0 - (i ? 1 : 0), h - 2, cols[i]);
	}
	if (dir >= 0 && h >= 8) {          /* direction triangle at the left end, on top of the segments */
		int th = h >= 12 ? 7 : 5, tw2 = th + 2;
		for (int j = 0; j < th; j++) {
			int bw = dir ? tw2 - j * tw2 / th : 2 + j * tw2 / th;
			if (bw < 1) bw = 1;
			lcd_fill(l, x + 3 + (tw2 - bw) / 2, y + (h - th) / 2 + j, bw, 1, RGB(10, 12, 16));
		}
	}
	if (showval && t->valid && h >= 12) {          /* the direction's total speed, dark on the segments */
		char num[16], val[24];
		const char *unit;
		fmt_rate(dir ? t->down : t->up, num, sizeof(num), &unit);
		snprintf(val, sizeof(val), "%s %s", num, unit);
		draw_halo(l, &FONT_T12, x + 15, y + (h - FONT_T12.h) / 2, val, RGB(10, 12, 16));
	}
}

static void draw_share_bar(lcd_t *l, const traffic_t *t, int by_total, int x, int y, int w, int h)
{
	draw_share_bar_m(l, t, by_total ? BM_TOTAL : BM_RATE, x, y, w, h, -1, 0);
}

/* Directional total bar with the total written inside it (down or up bytes). */
static void draw_total_bar(lcd_t *l, const traffic_t *t, int down, int x, int y, int w, int h)
{
	draw_share_bar_m(l, t, down ? BM_DTOTAL : BM_UTOTAL, x, y, w, h, -1, 0);
	if (!t->valid) return;
	char val[24];
	fmt_bytes(down ? t->dtot : t->utot, val, sizeof(val));
	const font_t *f = h >= 20 ? &FONT_T16B : &FONT_T12;
	int x0 = x + 5, ty = y + (h - f->h) / 2;
	draw_tri(l, x0, ty + (f->h - 10) / 2, down, RGB(10, 12, 16));
	draw_halo(l, f, x0 + 18, ty, val, RGB(10, 12, 16));
}

/* Ring split by client; the centre holds the total speed in the largest font that fits inside. */
static void draw_donut(lcd_t *l, const traffic_t *t, int cx, int cy, int ro)
{
	int ri = ro * 62 / 100, idx[MAX_CL], n = traffic_rank(t, 0, idx), shown = n > BAR_MAX ? BAR_MAX : n;
	double tot = 0, vals[BAR_MAX + 1] = { 0 };
	uint32_t cols[BAR_MAX + 1];
	for (int i = 0; i < n; i++) {
		double v = t->c[idx[i]].rate;
		if (v <= 0) continue;
		int slot = i < shown ? i : BAR_MAX;
		vals[slot] += v;
		cols[slot] = slot == BAR_MAX ? bar_color(C_OTHER) : bar_color(PALETTE[t->c[idx[i]].color]);
		tot += v;
	}
	for (int y = cy - ro; y <= cy + ro; y++)
		for (int x = cx - ro; x <= cx + ro; x++) {
			double dx = x - cx, dy = y - cy, d = sqrt(dx * dx + dy * dy);
			if (d > ro || d < ri) continue;
			uint32_t c = C_TRACK;
			if (tot > 0) {
				double a = atan2(dx, -dy), f, acc = 0;
				if (a < 0) a += 2 * M_PI;
				f = a / (2 * M_PI) * tot;
				for (int i = 0; i <= BAR_MAX; i++) {
					if (vals[i] <= 0) continue;
					acc += vals[i];
					if (f < acc || i == BAR_MAX) { c = cols[i]; break; }
				}
			}
			lcd_pixel(l, x, y, c);
		}
	char num[16];
	const char *unit;
	fmt_rate(t->down + t->up, num, sizeof(num), &unit);
	int inner = 2 * ri * 85 / 100;
	const font_t *f = lcd_text_w(&FONT_T24B, num, 1) <= inner && ri >= 22 ? &FONT_T24B : &FONT_T16B;
	if (lcd_text_w(f, num, 1) > inner) f = &FONT_T12;
	int with_unit = f->h + 12 + 2 <= inner;
	int ty = cy - (with_unit ? (f->h + 14) / 2 : f->h / 2);
	lcd_draw(l, f, cx - lcd_text_w(f, num, 1) / 2, ty, num, C_WHITE, LCD_TRANSPARENT, 1);
	if (with_unit) lcd_draw(l, &FONT_T12, cx - lcd_text_w(&FONT_T12, unit, 1) / 2, ty + f->h + 2, unit, C_DIM, LCD_TRANSPARENT, 1);
}

#define LEGEND_ROW 17

static uint32_t dim_color(uint32_t c, int pct)
{
	return RGB(((c >> 16) & 255) * pct / 100, ((c >> 8) & 255) * pct / 100, (c & 255) * pct / 100);
}

/* Rows to draw for a legend of at most `rows` lines (the last may be "+k others"). */
static int legend_rows(const traffic_t *t, int by_total, int rows)
{
	int idx[MAX_CL], n = traffic_rank(t, by_total, idx);
	if (n == 0) return 1;
	return n > rows ? rows : n;
}

/* Legend rows: chip, name, value. Optional background bar behind the name (bg: 1 speed,
 * 2 total, 3 signal, 4 signal+speed, 5 signal+total; the signal part is a bright 1 px line
 * at the row's bottom edge, the rest a dim bar in the client's colour, widest client = full
 * width). Optional fit (1 gentle, 2 compact): when clients overflow, squeeze the row pitch,
 * then (compact only) the font, to show more of them in the same height. */
static void draw_legend(lcd_t *l, const win_t *w, const traffic_t *t, int by_total, int rows, int y, int h, int name_len, int bg, int fit, int peak, int split)
{
	(void)rows;   /* row count comes from h, the height widget_h reserved */
	static const int PITCH[3] = { 17, 15, 13 };
	span_t sp = span_for(w, y, LEGEND_ROW);
	int idx[MAX_CL], n = traffic_rank(t, by_total, idx);
	if (n == 0) {
		lcd_draw(l, &FONT_T16, sp.x0, y, t->valid ? "No clients" : "No data", C_DIM, LCD_TRANSPARENT, 1);
		return;
	}
	int maxstep = fit >= 2 ? 2 : fit == 1 ? 1 : 0, step = 0;
	while (step < maxstep && n > h / PITCH[step]) step++;
	int pitch = PITCH[step], r = n > h / pitch ? h / pitch : n;
	const font_t *nf = step >= 2 ? &FONT_T12 : &FONT_T16, *vf = step >= 2 ? &FONT_T12 : &FONT_T16B;
	int named = n > r ? r - 1 : n;        /* leave the last row for "+k others" */
	int bgmode = bg == 2 || bg == 5 ? BM_TOTAL : BM_RATE, hasbg = bg == 1 || bg == 2 || bg == 4 || bg == 5;
	double bgmax = 0, maxd = 0, maxu = 0;          /* bgmax: what the plain bar divides by; maxd/maxu: per direction (split bar) */
	if (hasbg) for (int i = 0; i < named; i++) {
		const cl_t *cc = &t->c[idx[i]];
		double v = client_val(cc, bgmode), dv = bgmode == BM_TOTAL ? cc->dtot : cc->drate, uv = bgmode == BM_TOTAL ? cc->utot : cc->urate;
		if (v > bgmax) bgmax = v;
		if (dv > maxd) maxd = dv;
		if (uv > maxu) maxu = uv;
	}
	if (peak && hasbg && bgmode == BM_RATE && t->valid) {
		/* scale by the fastest rate seen so far instead of the current leader (who is always
		 * full width); the peak is forgotten slowly, halved every 10 minutes */
		static double pk_r, pk_d, pk_u;
		static time_t pk_t;
		time_t now = time(NULL);
		if (!pk_t) pk_t = now;
		while (now - pk_t >= 600) { pk_r /= 2; pk_d /= 2; pk_u /= 2; pk_t += 600; }
		for (int i = 0; i < t->n; i++) {
			if (t->c[i].rate > pk_r) pk_r = t->c[i].rate;
			if (t->c[i].drate > pk_d) pk_d = t->c[i].drate;
			if (t->c[i].urate > pk_u) pk_u = t->c[i].urate;
		}
		if (pk_r > bgmax) bgmax = pk_r;
		if (pk_d > maxd) maxd = pk_d;
		if (pk_u > maxu) maxu = pk_u;
	}
	char nm[48], val[16];
	for (int i = 0; i < r; i++, y += pitch) {
		sp = span_for(w, y, pitch);          /* rows near a corner are narrower */
		if (i < named) {
			const cl_t *c = &t->c[idx[i]];
			fmt_short(by_total ? c->total : c->rate, val, sizeof(val));
			int vw = lcd_text_w(vf, val, 1), room = sp.x1 + 1 - sp.x0 - 12 - vw - 4;
			if (room > (name_len < 12 ? 12 : name_len) * nf->w) room = (name_len < 12 ? 12 : name_len) * nf->w;
			if (room < 24) nm[0] = 0;                 /* no room for even a short name: chip and value only */
			else ellipsize(nf, c->name, room, 0, nm, sizeof(nm));
			int bw = 0;
			if (hasbg && bgmax > 0) {                 /* background bar: this client's share of the widest */
				uint32_t col = dim_color(bar_color(PALETTE[c->color]), 65);
				if (split && (maxd > 0 || maxu > 0)) {          /* upload: thin darker strip on top; download below, no gap */
					int bh = pitch - 1, uh = bh / 3, spanw = sp.x1 + 1 - sp.x0;
					double dv = bgmode == BM_TOTAL ? c->dtot : c->drate, uv = bgmode == BM_TOTAL ? c->utot : c->urate;
					double den = maxd > maxu ? maxd : maxu;          /* one scale for both: 30 down / 10 up shows up at a third */
					int uw = spanw * (int)(uv * 100 / den) / 100;
					int dw = spanw * (int)(dv * 100 / den) / 100;
					if (uh < 2) uh = 2;
					if (uw > 0) lcd_fill(l, sp.x0, y, uw, uh, dim_color(bar_color(PALETTE[c->color]), 92));   /* upload brighter, download as-is */
					if (dw > 0) lcd_fill(l, sp.x0, y + uh, dw, bh - uh, col);
					bw = uw > dw ? uw : dw;
				} else {
					bw = (sp.x1 + 1 - sp.x0) * (int)(client_val(c, bgmode) * 100 / bgmax) / 100;
					if (bw > 0) lcd_fill(l, sp.x0, y, bw, pitch - 1, col);
				}
			}
			/* text that sits on the bright bar turns dark, like the values on the direction bars */
			uint32_t namecol = bw > 12 + lcd_text_w(nf, nm, 1) / 2 ? RGB(10, 12, 16) : C_WHITE;
			uint32_t valcol = bw > sp.x1 - sp.x0 - vw - 2 ? RGB(10, 12, 16) : C_DIM;
			if ((bg == 3 || bg == 4 || bg == 5) && c->has_signal) {   /* signal: 1 px line, length and colour = quality */
				int sq = sig_pct(c->signal);
				lcd_fill(l, sp.x0, y + pitch - 2, (sp.x1 + 1 - sp.x0) * sq / 100, 1, sq >= 66 ? C_OK : sq >= 33 ? C_WARN : C_BAD);
			}
			int cs = step >= 2 ? 6 : 8;
			lcd_rrect(l, sp.x0, y + (pitch - cs) / 2, cs, cs, 2, PALETTE[c->color]);
			draw_halo(l, nf, sp.x0 + 12, y + (pitch - nf->h) / 2, nm, namecol);
			draw_halo(l, vf, sp.x1 + 1 - vw, y + (pitch - vf->h) / 2, val, valcol);
		} else {
			double sum = 0;
			for (int j = named; j < n; j++) sum += by_total ? t->c[idx[j]].total : t->c[idx[j]].rate;
			char more[24];
			snprintf(more, sizeof(more), "+%d more", n - named);
			fmt_short(sum, val, sizeof(val));
			ellipsize(nf, more, sp.x1 + 1 - sp.x0 - 12 - lcd_text_w(vf, val, 1) - 4, 0, nm, sizeof(nm));
			int cs = step >= 2 ? 6 : 8;
			lcd_rrect(l, sp.x0, y + (pitch - cs) / 2, cs, cs, 2, C_OTHER);
			lcd_draw(l, nf, sp.x0 + 12, y + (pitch - nf->h) / 2, nm, C_DIM, LCD_TRANSPARENT, 1);
			lcd_draw(l, vf, sp.x1 + 1 - lcd_text_w(vf, val, 1), y + (pitch - vf->h) / 2, val, C_DIM, LCD_TRANSPARENT, 1);
		}
	}
}

/* One speed line: arrow, number, unit - the whole group centred in the span, the unit in
 * the legend's value size (16) when that fits and 12 otherwise, bottoms aligned. */
static void draw_speed_line(lcd_t *l, span_t sp, int y, int h, int size, int down, double bps, int valid)
{
	char num[16];
	const char *unit;
	const font_t *f = size >= 24 ? &FONT_T24B : &FONT_T16B;
	if (valid) fmt_rate(bps, num, sizeof(num), &unit);
	else { copy(num, sizeof(num), "--"); unit = ""; }
	const font_t *uf = &FONT_T16;
	int gw = 18 + lcd_text_w(f, num, 1) + 4 + lcd_text_w(uf, unit, 1);
	if (gw > sp.x1 + 1 - sp.x0) { uf = &FONT_T12; gw = 18 + lcd_text_w(f, num, 1) + 4 + lcd_text_w(uf, unit, 1); }
	int x0 = sp.x0 + (sp.x1 + 1 - sp.x0 - gw) / 2;
	if (x0 < sp.x0) x0 = sp.x0;
	draw_tri(l, x0, y + (h - 10) / 2, down, down ? C_DOWN : C_UP);
	int x = lcd_draw(l, f, x0 + 18, y + (h - f->h) / 2, num, C_WHITE, LCD_TRANSPARENT, 1);
	lcd_draw(l, uf, x + 4, y + (h + f->h) / 2 - uf->h - 1, unit, C_DIM, LCD_TRANSPARENT, 1);
}

/* Down and up speed on one line: arrow number unit, twice, the group centred. */
static void draw_speed1(lcd_t *l, span_t sp, int y, int h, const traffic_t *t)
{
	char n1[16], n2[16];
	const char *u1, *u2;
	if (t->valid) { fmt_rate(t->down, n1, sizeof(n1), &u1); fmt_rate(t->up, n2, sizeof(n2), &u2); }
	else { copy(n1, sizeof(n1), "--"); copy(n2, sizeof(n2), "--"); u1 = u2 = ""; }
	int shared = t->valid && u1 == u2;               /* same unit: print it once, after the up value */
	const font_t *f = &FONT_T16B, *uf = &FONT_T12;
	int w1 = 16 + lcd_text_w(f, n1, 1) + (shared ? 0 : 3 + lcd_text_w(uf, u1, 1));
	int w2 = 16 + lcd_text_w(f, n2, 1) + 3 + lcd_text_w(uf, u2, 1);
	if (w1 + 8 + w2 > sp.x1 + 1 - sp.x0) {           /* both directions at 16 px do not always fit */
		f = &FONT_T12;
		w1 = 16 + lcd_text_w(f, n1, 1) + (shared ? 0 : 3 + lcd_text_w(uf, u1, 1));
		w2 = 16 + lcd_text_w(f, n2, 1) + 3 + lcd_text_w(uf, u2, 1);
	}
	int x = sp.x0 + (sp.x1 + 1 - sp.x0 - (w1 + 8 + w2)) / 2;
	if (x < sp.x0) x = sp.x0;
	int ty = y + (h - f->h) / 2;
	draw_tri(l, x, y + (h - 10) / 2, 1, C_DOWN);
	int xe = lcd_draw(l, f, x + 16, ty, n1, C_WHITE, LCD_TRANSPARENT, 1);
	if (!shared) lcd_draw(l, uf, xe + 3, y + (h + f->h) / 2 - uf->h - 1, u1, C_DIM, LCD_TRANSPARENT, 1);
	x += w1 + 8;
	draw_tri(l, x, y + (h - 10) / 2, 0, C_UP);
	xe = lcd_draw(l, f, x + 16, ty, n2, C_WHITE, LCD_TRANSPARENT, 1);
	lcd_draw(l, uf, xe + 3, y + (h + f->h) / 2 - uf->h - 1, u2, C_DIM, LCD_TRANSPARENT, 1);
}

/* Label, horizontal bar, value - the resource-row look. The label column fits the widest
 * label ("Signal") so bars align when such rows are stacked. */
/* Per-core rows: the bar area is split into one full-width strip per core (1 px gaps),
 * each strip filling with that core's utilisation; aggregate % stays on the right. */
static void draw_core_rows(lcd_t *l, span_t sp, int y, int h, int size, const res_t *r)
{
	const font_t *lf = size >= 16 ? &FONT_T16 : &FONT_T12, *vf = size >= 16 ? &FONT_T16B : &FONT_T12;
	char val[8];
	snprintf(val, sizeof(val), "%d%%", r->cpu);
	int lw = lcd_text_w(lf, "Signal", 1) + 4, vw = lcd_text_w(vf, val, 1), bw = sp.x1 + 1 - sp.x0 - lw - vw - 6;
	lcd_draw(l, lf, sp.x0, y + (h - lf->h) / 2, "CPU", C_DIM, LCD_TRANSPARENT, 1);
	if (bw >= 24 && r->ncores > 0) {
		int n = r->ncores, ah = size, sh = (ah - (n - 1)) / n;
		if (sh < 2) sh = 2;
		int by = y + (h - (sh * n + (n - 1))) / 2;
		for (int i = 0; i < n; i++) {
			int pct = r->core[i] > 100 ? 100 : r->core[i];
			lcd_fill(l, sp.x0 + lw, by + i * (sh + 1), bw, sh, C_TRACK);
			if (bw * pct / 100 >= 2) lcd_fill(l, sp.x0 + lw, by + i * (sh + 1), bw * pct / 100, sh, pct < 50 ? C_OK : pct < 80 ? C_WARN : C_BAD);
		}
	}
	lcd_draw(l, vf, sp.x1 + 1 - vw, y + (h - vf->h) / 2, val, C_WHITE, LCD_TRANSPARENT, 1);
}

static void draw_bar_row(lcd_t *l, span_t sp, int y, int h, int size, const char *label, int pct, const char *val, uint32_t fillc)
{
	/* size < 16 means the small row text (also chosen for a whole screen when its rows had to shrink) */
	const font_t *lf = size >= 16 ? &FONT_T16 : &FONT_T12, *vf = size >= 16 ? &FONT_T16B : &FONT_T12;
	int lw = lcd_text_w(lf, "Signal", 1) + 4, vw = lcd_text_w(vf, val, 1), bw = sp.x1 + 1 - sp.x0 - lw - vw - 6;
	lcd_draw(l, lf, sp.x0, y + (h - lf->h) / 2, label, C_DIM, LCD_TRANSPARENT, 1);
	if (bw >= 12) {
		int fw = bw * (pct > 100 ? 100 : pct) / 100, bh = size >= 16 ? 8 : 6;
		lcd_rrect(l, sp.x0 + lw, y + (h - bh) / 2, bw, bh, 3, C_TRACK);
		if (fw >= 4) lcd_rrect(l, sp.x0 + lw, y + (h - bh) / 2, fw, bh, 3, fillc);
	}
	lcd_draw(l, vf, sp.x1 + 1 - lcd_text_w(vf, val, 1), y + (h - vf->h) / 2, val, C_WHITE, LCD_TRANSPARENT, 1);
}

/* Resource row: bar (green, amber above 60 %, red above 85 %), value in percent. */
static void draw_resource(lcd_t *l, span_t sp, int y, int h, int size, const char *label, int pct, uint32_t track)
{
	(void)track;
	char val[8];
	snprintf(val, sizeof(val), "%d%%", pct);
	draw_bar_row(l, sp, y, h, size, label, pct, val, pct < 60 ? C_OK : pct < 85 ? C_WARN : C_BAD);
}

/* CPU and RAM on one line, each in its own half: label, small bar, percent. On screens that are
 * not a stack of resource rows the aligned label column of draw_bar_row just wastes room. */
static void draw_cpuram(lcd_t *l, span_t sp, int y, int h, int size, const res_t *r)
{
	const font_t *lf = size >= 16 ? &FONT_T16 : &FONT_T12, *vf = size >= 16 ? &FONT_T16B : &FONT_T12;
	const struct { const char *lab; int pct; } it[2] = { { "CPU", r->cpu }, { "RAM", r->ram } };
	int mid = (sp.x0 + sp.x1 + 1) / 2, bh = size >= 16 ? 8 : 6;
	for (int i = 0; i < 2; i++) {
		int x0 = i ? mid + 3 : sp.x0, x1 = i ? sp.x1 : mid - 4;
		char val[8];
		snprintf(val, sizeof(val), "%d%%", it[i].pct);
		int vw = lcd_text_w(vf, val, 1);
		lcd_draw(l, lf, x0, y + (h - lf->h) / 2, it[i].lab, C_DIM, LCD_TRANSPARENT, 1);
		int bx = x0 + lcd_text_w(lf, it[i].lab, 1) + 3, bw = x1 + 1 - vw - 3 - bx;
		if (bw >= 8) {
			int pct = it[i].pct > 100 ? 100 : it[i].pct, fw = bw * pct / 100;
			lcd_rrect(l, bx, y + (h - bh) / 2, bw, bh, 3, C_TRACK);
			if (fw >= 3) lcd_rrect(l, bx, y + (h - bh) / 2, fw, bh, 3, pct < 60 ? C_OK : pct < 85 ? C_WARN : C_BAD);
		}
		lcd_draw(l, vf, x1 + 1 - vw, y + (h - vf->h) / 2, val, C_WHITE, LCD_TRANSPARENT, 1);
	}
}

/* CPU bar split into one segment per core (green below 50 %, amber to 80 %, red above). */
static void draw_core_bar(lcd_t *l, span_t sp, int y, int h, int size, const res_t *r)
{
	const font_t *lf = size >= 16 ? &FONT_T16 : &FONT_T12, *vf = size >= 16 ? &FONT_T16B : &FONT_T12;
	char val[8];
	snprintf(val, sizeof(val), "%d%%", r->cpu);
	int lw = lcd_text_w(lf, "Signal", 1) + 4, vw = lcd_text_w(vf, val, 1), bw = sp.x1 + 1 - sp.x0 - lw - vw - 6;
	lcd_draw(l, lf, sp.x0, y + (h - lf->h) / 2, "CPU", C_DIM, LCD_TRANSPARENT, 1);
	if (bw >= 24 && r->ncores > 0) {
		int n = r->ncores, bh = size >= 16 ? 8 : 6, by = y + (h - bh) / 2;
		for (int i = 0; i < n; i++) {
			int pct = r->core[i] > 100 ? 100 : r->core[i];
			int sx = sp.x0 + lw + bw * i / n, sw = bw * (i + 1) / n - bw * i / n;   /* segments tile the bar, no gaps */
			lcd_fill(l, sx, by, sw, bh, C_TRACK);
			if (sw * pct / 100 >= 2) lcd_fill(l, sx, by, sw * pct / 100, bh, pct < 50 ? C_OK : pct < 80 ? C_WARN : C_BAD);
		}
	}
	lcd_draw(l, vf, sp.x1 + 1 - vw, y + (h - vf->h) / 2, val, C_WHITE, LCD_TRANSPARENT, 1);
}

/* Clock face for the icon form of uptime (shared lines, where the "Uptime" label does not fit). */
static void draw_clock(lcd_t *l, int x, int y, uint32_t c)
{
	int cx = x + 8, cy = y + 8;
	for (int a = 0; a < 64; a++) {
		double t = a * M_PI / 32;
		lcd_fill(l, cx + (int)lround(7 * cos(t)) - 1, cy + (int)lround(7 * sin(t)) - 1, 2, 2, c);
	}
	lcd_fill(l, cx - 1, cy - 5, 2, 6, c);
	lcd_fill(l, cx - 1, cy - 1, 6, 2, c);
}

/* Globe for the online-status widget: outline, equator, one meridian, two parallels. */
static void draw_globe(lcd_t *l, int cx, int cy, int r, uint32_t c)
{
	for (int a = 0; a < 96; a++) {
		double t = a * M_PI / 48;
		lcd_fill(l, cx + (int)lround(r * cos(t)) - 1, cy + (int)lround(r * sin(t)) - 1, 2, 2, c);
	}
	lcd_fill(l, cx - r, cy - 1, 2 * r, 2, c);
	for (int a = 0; a < 48; a++) {
		double t = a * M_PI / 24;
		lcd_fill(l, cx + (int)lround(r / 2 * cos(t)) - 1, cy + (int)lround(r * sin(t)) - 1, 2, 2, c);
	}
	if (r >= 12) {          /* parallels only where they do not mush together */
		int pw = (int)lround(r * 0.86);
		lcd_fill(l, cx - pw, cy - r / 2 - 1, 2 * pw, 2, c);
		lcd_fill(l, cx - pw, cy + r / 2 - 1, 2 * pw, 2, c);
	}
}

/* Wi-Fi fan: a dot and up to 4 arcs above it. Lit arcs by signal quality; with `shadow` set,
 * every stroke is first drawn offset in dark colour (readable on the bright signal backfill). */
static void draw_fan(lcd_t *l, int cx, int base, int size, int pct, uint32_t col, uint32_t dimc, int shadow)
{
	/* small fans get three arcs with wider spacing: four arcs 20% apart mush together below ~20 px */
	int narcs = size < 20 ? 3 : 4, step = narcs == 3 ? 30 : 20;
	int lit = (pct * narcs + 50) / 100, thick = size >= 32 ? 3 : 2;
	for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
		uint32_t lc = pass ? col : RGB(12, 16, 12), dc = pass ? dimc : RGB(12, 16, 12);
		int o = pass ? 0 : 1;
		lcd_disc(l, cx + o, base - size / 8 + o, size / 10 < 2 ? 2 : size / 10, pct > 0 ? lc : dc);
		for (int i = 0; i < narcs; i++) {
			int r = size * (38 + i * step) / 100;
			uint32_t c = i < lit ? lc : dc;
			for (int a = 0; a <= 32; a++) {
				double t = M_PI * (-0.89 + 0.78 * a / 32.0);   /* sweep above the dot (screen y is down) */
				lcd_fill(l, cx + (int)lround(r * cos(t)) + o, base - size / 8 + (int)lround(r * sin(t)) + o, thick, thick, c);
			}
		}
	}
}

#define UPL_COLORS 3
static const uint32_t UPL_COLOR[UPL_COLORS] = { RGB(0, 150, 255), RGB(255, 150, 30), RGB(230, 70, 200) };

/* Name of uplink n for display. */
static const char *uplink_label(const status_t *s, int n)
{
	if (!strcmp(s->ups[n].type, "wifi")) return s->ups[n].name[0] ? s->ups[n].name : "Wi-Fi";
	if (!strcmp(s->ups[n].type, "usb")) return "USB";
	return "Ethernet";
}

/* Per uplink: name and share of the current traffic, then a bar in the uplink's colour. */
static void draw_multiwan(lcd_t *l, const win_t *w, const status_t *s, const traffic_t *t, int size, int y)
{
	const font_t *lf = size >= 16 ? &FONT_T16B : &FONT_T12;
	int bh = size >= 16 ? 8 : 6, row = lf->h + 2 + bh + 4;
	double tot = 0, v[3] = { 0 };
	for (int i = 0; i < s->nups; i++) {
		for (int j = 0; j < t->nu; j++)
			if (!strcmp(t->u[j].dev, s->ups[i].dev)) v[i] = t->u[j].down + t->u[j].up;
		tot += v[i];
	}
	for (int i = 0; i < s->nups; i++, y += row) {
		span_t sp = span_for(w, y, row);
		int pct = tot > 0 ? (int)(v[i] * 100 / tot + 0.5) : 0;
		char val[16], nm[64];
		if (!t->valid) copy(val, sizeof(val), "--");
		else if (tot <= 0) copy(val, sizeof(val), "idle");
		else if (pct == 0 && i > 0 && s->ups[i].metric > s->ups[0].metric) copy(val, sizeof(val), "backup");
		else snprintf(val, sizeof(val), "%d%%", pct);
		int vw = lcd_text_w(lf, val, 1);
		ellipsize(lf, uplink_label(s, i), sp.x1 + 1 - sp.x0 - vw - 6, 0, nm, sizeof(nm));
		lcd_draw(l, lf, sp.x0, y, nm, C_WHITE, LCD_TRANSPARENT, 1);
		lcd_draw(l, lf, sp.x1 + 1 - vw, y, val, C_DIM, LCD_TRANSPARENT, 1);
		int bw = sp.x1 - sp.x0 + 1, by = y + lf->h + 2;
		lcd_rrect(l, sp.x0, by, bw, bh, 3, C_TRACK);
		if (tot > 0 && pct > 0 && bw * pct / 100 >= 4) lcd_rrect(l, sp.x0, by, bw * pct / 100, bh, 3, UPL_COLOR[i % UPL_COLORS]);
	}
}

static int multiwan_h(const status_t *s, int size)
{
	return s->nups >= 2 ? s->nups * ((size >= 16 ? FONT_T16B.h : FONT_T12.h) + 2 + (size >= 16 ? 8 : 6) + 4) : 0;
}

/* ---- uplinks widget: one icon per uplink, state colour, two 1 px traffic-share lines ---- */

typedef struct { char label[32]; int wifi; int state; int pct; int signal, has_signal; double down, up; } uview_t;
enum { UV_OK, UV_DIM, UV_NOROUTE, UV_OFF };

/* Display list: mwan3 interfaces when the collector heard from mwan3 (they are shown even
 * without a default route - that mismatch is the point), otherwise the route uplinks. */
static int build_uplinks(const status_t *s, const traffic_t *t, uview_t *v)
{
	int n = 0;
	if (s->nmw > 0) {
		for (int i = 0; i < s->nmw && n < 3; i++) {
			uview_t *u = &v[n];
			memset(u, 0, sizeof(*u));
			u->pct = s->mw[i].pct;
			int m = -1;
			for (int j = 0; j < s->nups; j++) if (!strcmp(s->ups[j].dev, s->mw[i].dev)) m = j;
			if (m >= 0) {
				copy(u->label, sizeof(u->label), uplink_label(s, m));
				u->wifi = !strcmp(s->ups[m].type, "wifi");
				u->signal = s->ups[m].signal; u->has_signal = s->ups[m].has_signal;
			} else {
				copy(u->label, sizeof(u->label), s->mw[i].ifname);
				u->wifi = !strncmp(s->mw[i].dev, "phy", 3) || !strncmp(s->mw[i].dev, "wlan", 4);
			}
			for (int j = 0; j < t->nu; j++) if (!strcmp(t->u[j].dev, s->mw[i].dev)) { u->down = t->u[j].down; u->up = t->u[j].up; }
			int off = !strcmp(s->mw[i].state, "offline") || !strcmp(s->mw[i].state, "down") || !strcmp(s->mw[i].state, "disabled");
			u->state = off ? UV_OFF : m < 0 ? UV_NOROUTE : s->mw[i].pct == 0 ? UV_DIM : UV_OK;
			n++;
		}
		/* route uplinks mwan3 does not manage (e.g. a second STA in the main table): show them too */
		for (int j = 0; j < s->nups && n < 3; j++) {
			int known = 0;
			for (int i = 0; i < s->nmw; i++) if (!strcmp(s->mw[i].dev, s->ups[j].dev)) known = 1;
			if (known) continue;
			uview_t *u = &v[n];
			memset(u, 0, sizeof(*u));
			u->pct = -1;
			copy(u->label, sizeof(u->label), uplink_label(s, j));
			u->wifi = !strcmp(s->ups[j].type, "wifi");
			u->signal = s->ups[j].signal; u->has_signal = s->ups[j].has_signal;
			for (int k = 0; k < t->nu; k++) if (!strcmp(t->u[k].dev, s->ups[j].dev)) { u->down = t->u[k].down; u->up = t->u[k].up; }
			u->state = u->down + u->up > 0 ? UV_OK : UV_DIM;
			n++;
		}
		return n;
	}
	for (int i = 0; i < s->nups && n < 3; i++) {
		uview_t *u = &v[n];
		memset(u, 0, sizeof(*u));
		u->pct = -1;
		copy(u->label, sizeof(u->label), uplink_label(s, i));
		u->wifi = !strcmp(s->ups[i].type, "wifi");
		u->signal = s->ups[i].signal; u->has_signal = s->ups[i].has_signal;
		for (int j = 0; j < t->nu; j++) if (!strcmp(t->u[j].dev, s->ups[i].dev)) { u->down = t->u[j].down; u->up = t->u[j].up; }
		u->state = u->down + u->up > 0 ? UV_OK : UV_DIM;
		n++;
	}
	return n;
}

static uint32_t upl_color(int state)
{
	switch (state) {
	case UV_OK: return C_OK;
	case UV_NOROUTE: return C_WARN;
	case UV_OFF: return C_BAD;
	default: return RGB(90, 110, 95);   /* standby: dim green-grey */
	}
}

/* Ethernet port: rounded outline, three pins, cable stub below. */
static void draw_port(lcd_t *l, int cx, int y, int size, uint32_t c)
{
	int w = size >= 24 ? 20 : 14, h = size >= 24 ? 14 : 10, x = cx - w / 2, py = y + (size - h) / 2 - 2;
	lcd_rrect(l, x, py, w, h, 3, c);
	lcd_rrect(l, x + 2, py + 2, w - 4, h - 4, 2, C_BG);
	for (int i = 0; i < 3; i++) lcd_fill(l, x + w / 4 + i * w / 4 - 1, py + 2, 2, h / 2, c);
	lcd_fill(l, cx - 2, py + h, 4, size >= 24 ? 5 : 3, c);
}

static int upl_pitch(int size) { return size >= 24 ? 20 : 16; }   /* label-only row pitch */

/* Uplink icons with state colours, optional labels (layout: one row per uplink, icon beside
 * the label; otherwise icons side by side above their labels), then two 1 px lines with the
 * observed download/upload split. Flags: l rows, g short WiFi/Eth labels, n no labels,
 * i no icons. The Wi-Fi fan is drawn at 3/5 size: at full size it dwarfs the port glyph. */
static void draw_uplinks(lcd_t *l, span_t sp, int y, int size, const status_t *s, const traffic_t *t, int layout, int generic, int noname, int noicon)
{
	uview_t v[3];
	int n = build_uplinks(s, t, v);
	if (n == 0) {
		lcd_draw(l, &FONT_T12, (sp.x0 + sp.x1 + 1) / 2 - lcd_text_w(&FONT_T12, "no uplink", 1) / 2, y + 2, "no uplink", C_BAD, LCD_TRANSPARENT, 1);
		return;
	}
	int split = 0, fs = size * 3 / 5;
	for (int i = 0; i < n; i++) if (v[i].pct > 0 && v[i].pct < 100) split = 1;
	int rowh = layout ? (noicon ? upl_pitch(size) : size) : (noicon ? 14 : size + (noname ? 0 : 14));
	for (int i = 0; i < n; i++) {
		uint32_t col = upl_color(v[i].state);
		char lab[40], out[40];
		copy(lab, sizeof(lab), generic ? (v[i].wifi ? "WiFi" : "Eth") : v[i].label);
		if (split && v[i].pct >= 0) snprintf(lab + strlen(lab), sizeof(lab) - strlen(lab), " %d%%", v[i].pct);
		if (layout) {
			int ry = y + i * rowh;
			const font_t *lf = size >= 24 ? &FONT_T16B : &FONT_T12, *lf2 = lf;
			if (!noname) {
				lf2 = ((int)strlen(lab) * lf->w > sp.x1 - sp.x0 - (noicon ? 0 : size + 6) && lf != &FONT_T12) ? &FONT_T12 : lf;   /* shrink before truncating */
				ellipsize(lf2, lab, sp.x1 - sp.x0 - (noicon ? 0 : size + 6), 0, out, sizeof(out));
			}
			/* the icon+label group is centred in the row (label alone / icon alone likewise) */
			int gw = (noicon ? 0 : size) + (noname ? 0 : (noicon ? 0 : 6) + lcd_text_w(lf2, out, 1));
			int gx = (sp.x0 + sp.x1 + 1 - gw) / 2, cx = gx + size / 2;
			if (!noicon) {
				if (v[i].wifi) draw_fan(l, cx, ry + size, fs, v[i].has_signal ? sig_pct(v[i].signal) : 100, col, C_TRACK, 0);
				else draw_port(l, cx, ry, size, col);
			}
			if (!noname) {
				/* the fan sits low in its row (dot at the bottom): centre the label on it, not on the row */
				int lyy = !noicon && v[i].wifi ? ry + size - (fs + fs / 8) / 2 - lf2->h / 2 : ry + (rowh - lf2->h) / 2;
				lcd_draw(l, lf2, gx + (noicon ? 0 : size + 6), lyy, out, col, LCD_TRANSPARENT, 1);
			}
		} else {
			int cw = (sp.x1 - sp.x0 + 1) / n, cx = sp.x0 + cw * i + cw / 2;
			if (!noicon) {
				if (v[i].wifi) draw_fan(l, cx, y + size, fs, v[i].has_signal ? sig_pct(v[i].signal) : 100, col, C_TRACK, 0);
				else draw_port(l, cx, y, size, col);
			}
			if (!noname) {
				ellipsize(&FONT_T12, lab, cw - 2, 0, out, sizeof(out));
				/* the port glyph ends above the row's bottom edge: its label sits closer than the fan's */
				lcd_draw(l, &FONT_T12, cx - lcd_text_w(&FONT_T12, out, 1) / 2, y + (noicon ? 1 : v[i].wifi ? size + 2 : size), out, col, LCD_TRANSPARENT, 1);
			}
		}
	}
	/* observed split: down line over up line, 1 px each, segments in the uplink colours */
	double dt = 0, ut = 0;
	for (int i = 0; i < n; i++) { dt += v[i].down; ut += v[i].up; }
	int lx0 = sp.x0 + 8, wdt = sp.x1 - lx0 + 1, ly = y + (layout ? n : 1) * rowh + 3;
	if (noname && noicon) ly = y + 1;
	for (int line = 0; line < 2; line++) {          /* down line over up line, marked at the left */
		double tot = line ? ut : dt, acc = 0;
		for (int j = 0; j < 3; j++) {
			int bw = line ? 2 + j * 2 : 5 - j * 2;
			lcd_fill(l, sp.x0 + (5 - bw) / 2, ly + line * 3 - 1 + j, bw, 1, C_DIM);
		}
		lcd_fill(l, lx0, ly + line * 3, wdt, 1, C_TRACK);
		for (int i = 0; i < n && tot > 0; i++) {
			double val = line ? v[i].up : v[i].down;
			int x0 = lx0 + (int)(acc / tot * wdt + 0.5);
			acc += val;
			int x1 = lx0 + (int)(acc / tot * wdt + 0.5);
			if (x1 > x0) lcd_fill(l, x0, ly + line * 3, x1 - x0, 1, upl_color(v[i].state));
		}
	}
}

static int uplinks_h(const status_t *s, const traffic_t *t, int size, int layout, int noname, int noicon)
{
	uview_t v[3];
	int n = build_uplinks(s, t, v);
	if (!n) return FONT_T12.h + 2;
	if (noname && noicon) return 8;
	int rowh = layout ? (noicon ? upl_pitch(size) : size) : (noicon ? 14 : size + (noname ? 0 : 14));
	return (layout ? n : 1) * rowh + 8 + 2;   /* rows + the two 1 px split lines */
}

/* ---------------------------------------------------------------------------------------- widgets */

typedef struct { const status_t *s; const traffic_t *tr; const res_t *res; const char *state; int backfill; int row_step; int name_len; } ctx_t;

#define PAD 2
#define RULE_H 6

/* Where a wrapped name breaks: after a '-' or ' ' in reach, hard break at the line width
 * otherwise. Returns the number of characters on the first line. */
static int ssid_break(const font_t *f, const char *s, int maxw)
{
	int maxc = maxw / f->w, len = (int)strlen(s);
	if (maxc < 4) maxc = 4;
	if (maxc > len) maxc = len;
	for (int i = maxc; i > maxc / 2; i--)
		if (s[i - 1] == '-' || s[i - 1] == ' ') return i;   /* break after the separator */
	return maxc;
}

/* Lines (1 or 2) the name needs at f, or 0 when even two lines would have to be cut. */
static int ssid_fit_lines(const font_t *f, const char *s, int maxw)
{
	if (lcd_text_w(f, s, 1) <= maxw) return 1;
	const char *rest = s + ssid_break(f, s, maxw);
	if (*rest == ' ') rest++;
	return lcd_text_w(f, rest, 1) <= maxw ? 2 : 0;
}

/* Font for a wrapped name: at the chosen size, unless shrink is on and the name does not fit
 * on two lines there - then step down the ladder until it does (smallest font: cut as before).
 * Sets *lines to the lines the name takes at the returned font. */
static const font_t *ssid_font(int size, const char *s, int maxw, int shrink, int *lines)
{
	int i = ladder_index(size);
	if (!shrink) {
		*lines = ssid_fit_lines(LADDER[i], s, maxw);
		if (!*lines) *lines = 2;
		return LADDER[i];
	}
	for (; i < NLADDER; i++) {
		int n = ssid_fit_lines(LADDER[i], s, maxw);
		if (n) { *lines = n; return LADDER[i]; }
	}
	*lines = 2;
	return LADDER[NLADDER - 1];
}

/* Height a widget needs; 0 = nothing to show right now (it takes no room). */
static int widget_h(const widget_t *wd, const ctx_t *c)
{
	if (wd->type == W_ROW) {           /* a shared line is as tall as its tallest visible member */
		int m = 0;
		for (int i = 0; i < wd->nsub; i++) {
			widget_t t = { .type = wd->sub[i].type, .size = wd->sub[i].size, .wrap = wd->sub[i].wrap, .shrink = wd->sub[i].shrink, .bg = wd->sub[i].bg, .fit = wd->sub[i].fit, .cores = wd->sub[i].cores, .layout = wd->sub[i].layout, .generic = wd->sub[i].generic, .noname = wd->sub[i].noname, .noicon = wd->sub[i].noicon, .showval = wd->sub[i].showval, .peak = wd->sub[i].peak, .split = wd->sub[i].split };
			int hh = widget_h(&t, c);
			if (hh > m) m = hh;
		}
		return m;
	}
	switch (wd->type) {
	case W_STATUS: return wd->size + PAD;
	case W_SSID: {
		if (!wd->wrap) return wd->size + PAD;
		/* wrapping: a second line only when the name at the chosen size is wider than a line;
		 * with shrink the two lines may be set in a smaller font, and take less room */
		const status_t *st = c->s;
		const char *nm = strcmp(st->up_state, "up") ? "No uplink" : !strcmp(st->up_type, "wifi") ? (st->up_name[0] ? st->up_name : "Wi-Fi") : !strcmp(st->up_type, "usb") ? "USB WAN" : "Ethernet";
		int lines;
		const font_t *f = ssid_font(wd->size, nm, 110, wd->shrink, &lines);
		return lines > 1 ? 2 * f->h + PAD : wd->size + PAD;
	}
	case W_DBM: {
		int wifi = c->s->has_signal && !strcmp(c->s->up_type, "wifi");
		return !strcmp(c->s->up_state, "up") && (wifi || c->s->speed > 0) ? wd->size + PAD : 0;
	}
	case W_BARS:
	case W_SIGBAR:
	case W_FAN:
	case W_SEGBAR:
		return !strcmp(c->s->up_state, "up") && c->s->has_signal && !strcmp(c->s->up_type, "wifi") ? wd->size + PAD : 0;
	case W_WANIP: return c->s->wan_ip[0] ? wd->size + PAD : 0;
	case W_LANIP: return c->s->lan_ip[0] ? wd->size + PAD : 0;
	case W_VPN: return strcmp(c->s->vpn_state, "none") && c->s->vpn_state[0] ? wd->size + PAD : 0;
	case W_RULE: return RULE_H;
	case W_SPEED: return 2 * (wd->size + PAD);
	case W_SPEEDBAR: case W_SPEEDDN: case W_SPEEDUP: case W_USAGEBAR: case W_USAGEDN: case W_USAGEUP: return wd->size + 1;   /* bars pack tighter */
	case W_LEGEND: return legend_rows(c->tr, 0, wd->size) * LEGEND_ROW;
	case W_LEGENDTOTAL: return legend_rows(c->tr, 1, wd->size) * LEGEND_ROW;
	case W_DONUT: return wd->size + PAD;
	case W_TEMP: return c->res->temp >= 0 ? wd->size + PAD : 0;
	case W_MULTIWAN: return multiwan_h(c->s, wd->size);
	case W_UPLINKS: return uplinks_h(c->s, c->tr, wd->size, wd->layout, wd->noname, wd->noicon);     /* never hidden: shows "no uplink" when empty */
	default: return wd->size + PAD;
	}
}

static void widget_draw(lcd_t *l, const win_t *w, const widget_t *wd, ctx_t *c, int y, int h);

/* SSID with wrapping: break to a second line (after a '-' or ' ' if there is one in reach,
 * hard break otherwise) instead of cutting the name; with shrink, step the font down first
 * when two lines at the chosen size would still have to be cut. */
static void draw_ssid_wrapped(lcd_t *l, span_t sp, int y, int h, int size, const char *s, uint32_t c, int shrink)
{
	int maxw = sp.x1 - sp.x0 + 1, cx = (sp.x0 + sp.x1 + 1) / 2, lines;
	const font_t *f = ssid_font(size, s, maxw, shrink, &lines);
	if (lines == 1) {
		lcd_draw(l, f, cx - lcd_text_w(f, s, 1) / 2, y + (h - f->h) / 2, s, c, LCD_TRANSPARENT, 1);
		return;
	}
	int br = ssid_break(f, s, maxw);
	char l1[48], l2[48];
	snprintf(l1, sizeof(l1), "%.*s", br, s);
	const char *rest = s + br;
	if (*rest == ' ') rest++;
	ellipsize(f, rest, maxw, 0, l2, sizeof(l2));
	int y0 = y + (h - 2 * f->h) / 2;
	lcd_draw(l, f, cx - lcd_text_w(f, l1, 1) / 2, y0, l1, c, LCD_TRANSPARENT, 1);
	lcd_draw(l, f, cx - lcd_text_w(f, l2, 1) / 2, y0 + f->h, l2, c, LCD_TRANSPARENT, 1);
}

static void widget_draw_sp(lcd_t *l, const win_t *w, const widget_t *wd, ctx_t *c, int y, int h, span_t sp)
{
	const status_t *s = c->s;
	int up = !strcmp(s->up_state, "up"), wifi = !strcmp(s->up_type, "wifi");
	char buf[64];

	if (wd->type == W_ROW) {
		/* visible members get equal cells of the span, each drawn centred in its cell */
		int vis[MAX_SUB], nv = 0;
		for (int i = 0; i < wd->nsub; i++) {
			widget_t t = { .type = wd->sub[i].type, .size = wd->sub[i].size, .wrap = wd->sub[i].wrap, .shrink = wd->sub[i].shrink, .bg = wd->sub[i].bg, .fit = wd->sub[i].fit, .cores = wd->sub[i].cores, .layout = wd->sub[i].layout, .generic = wd->sub[i].generic, .noname = wd->sub[i].noname, .noicon = wd->sub[i].noicon, .showval = wd->sub[i].showval, .peak = wd->sub[i].peak, .split = wd->sub[i].split };
			if (widget_h(&t, c)) vis[nv++] = i;
		}
		int width = sp.x1 - sp.x0 + 1, cw = nv ? width / nv : width;
		for (int k = 0; k < nv; k++) {
			widget_t t = { .type = wd->sub[vis[k]].type, .size = wd->sub[vis[k]].size, .wrap = wd->sub[vis[k]].wrap, .shrink = wd->sub[vis[k]].shrink, .bg = wd->sub[vis[k]].bg, .fit = wd->sub[vis[k]].fit, .cores = wd->sub[vis[k]].cores, .layout = wd->sub[vis[k]].layout, .generic = wd->sub[vis[k]].generic, .noname = wd->sub[vis[k]].noname, .noicon = wd->sub[vis[k]].noicon, .showval = wd->sub[vis[k]].showval, .peak = wd->sub[vis[k]].peak, .split = wd->sub[vis[k]].split };
			span_t cell = { sp.x0 + k * cw, k == nv - 1 ? sp.x1 : sp.x0 + (k + 1) * cw - 1 };
			widget_draw_sp(l, w, &t, c, y, h, cell);
		}
		return;
	}

	switch (wd->type) {
	case W_SSID:
		if (!up) text_centered(l, w, sp, y, h, wd->size, "No uplink", C_BAD, 0);
		else if (wifi && wd->wrap && sp.x1 - sp.x0 + 1 >= 90) draw_ssid_wrapped(l, sp, y, h, wd->size, s->up_name[0] ? s->up_name : "Wi-Fi", C_WHITE, wd->shrink);
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
	case W_BARS: {
		/* 5 ascending bars, lit by signal quality (same scale as the backfill) */
		int p = sig_pct(s->signal), nb = 5, lit = p <= 0 ? 0 : (p * nb + 99) / 100;
		uint32_t col = c->backfill ? C_WHITE : p >= 50 ? C_OK : p >= 25 ? C_WARN : C_BAD;
		int bw = wd->size >= 24 ? 6 : 4, bg = 3, bx = (sp.x0 + sp.x1 + 1) / 2 - (nb * bw + (nb - 1) * bg) / 2, base = y + (h + wd->size) / 2;
		for (int i = 0; i < nb; i++) {
			int bh = wd->size * (i + 1) / nb;
			if (c->backfill)          /* white bars on the bright fill need a dark outline to read */
				lcd_fill(l, bx + i * (bw + bg) - 1, base - bh - 1, bw + 2, bh + 2, RGB(12, 16, 12));
			lcd_fill(l, bx + i * (bw + bg), base - bh, bw, bh, i < lit ? col : C_TRACK);
		}
		break;
	}
	case W_FAN: {
		int p = sig_pct(s->signal);
		uint32_t col = c->backfill ? C_WHITE : p >= 50 ? C_OK : p >= 25 ? C_WARN : C_BAD;
		draw_fan(l, (sp.x0 + sp.x1 + 1) / 2, y + (h + wd->size) / 2, wd->size, p, col, C_TRACK, c->backfill);
		break;
	}
	case W_SEGBAR: {
		/* Stock-style segmented bar: 10 rounded segments across the whole line, filled by quality. */
		int p = sig_pct(s->signal), nseg = 10, lit = (p * nseg + 50) / 100;
		if (p > 0 && !lit) lit = 1;
		uint32_t col = c->backfill ? C_WHITE : p >= 50 ? C_OK : p >= 25 ? C_WARN : C_BAD;
		int width = sp.x1 - sp.x0 + 1, gap = 3, sw = (width - (nseg - 1) * gap) / nseg, y0 = y + (h - wd->size) / 2;
		for (int i = 0; i < nseg; i++) {
			if (c->backfill && i < lit)
				lcd_rrect(l, sp.x0 + i * (sw + gap) - 1, y0 - 1, sw + 2, wd->size + 2, 3, RGB(12, 16, 12));
			lcd_rrect(l, sp.x0 + i * (sw + gap), y0, sw, wd->size, 2, i < lit ? col : C_TRACK);
		}
		break;
	}
	case W_SIGBAR: {
		int p = sig_pct(s->signal);
		uint32_t col = p >= 50 ? C_OK : p >= 25 ? C_WARN : C_BAD;
		snprintf(buf, sizeof(buf), "%d dBm", s->signal);
		if (wd->size >= 24) {
			/* tall bar with the value on top of it */
			int bw2 = sp.x1 - sp.x0 + 1, by = y + (h - wd->size) / 2, fw = bw2 * p / 100;
			const font_t *f = lcd_text_w(&FONT_T16B, buf, 1) <= bw2 - 8 ? &FONT_T16B : &FONT_T12;
			lcd_rrect(l, sp.x0, by, bw2, wd->size, 4, C_TRACK);
			if (fw >= 4) lcd_rrect(l, sp.x0, by, fw, wd->size, 4, col);
			lcd_draw(l, f, (sp.x0 + sp.x1 + 1) / 2 - lcd_text_w(f, buf, 1) / 2, by + (wd->size - f->h) / 2, buf, C_WHITE, LCD_TRANSPARENT, 1);
		} else {
			/* slim, resource-style: percent value, so the bar gets the room (dBm is on dbm/tall) */
			char val[8];
			snprintf(val, sizeof(val), "%d%%", p);
			draw_bar_row(l, sp, y, h, c->row_step ? 12 : wd->size, "Signal", p, val, col);
		}
		break;
	}
	case W_CLIENTS: {
		const font_t *f = LADDER[ladder_index(wd->size)];
		int sc = wd->size >= 32 ? 2 : 1, iw = 16 * sc;
		snprintf(buf, sizeof(buf), "%d", s->clients);
		int tw = lcd_text_w(f, buf, 1), x = (sp.x0 + sp.x1 + 1) / 2 - (iw + 6 + tw) / 2;
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
		if (sp.x1 - sp.x0 + 1 < 90) {
			/* shared line: the label does not fit, a clock icon takes its place */
			fmt_uptime_short(s->uptime, buf, sizeof(buf));
			const font_t *f = wd->size >= 16 && lcd_text_w(&FONT_T16B, buf, 1) + 20 <= sp.x1 - sp.x0 + 1 ? &FONT_T16B : &FONT_T12;
			int tw = lcd_text_w(f, buf, 1), icon = 16 + 4 + tw <= sp.x1 - sp.x0 + 1;   /* no room: value only */
			int x = (sp.x0 + sp.x1 + 1) / 2 - ((icon ? 20 : 0) + tw) / 2;
			if (l && icon) draw_clock(l, x, y + (h - 16) / 2, C_WHITE);
			lcd_draw(l, f, x + (icon ? 20 : 0), y + (h - f->h) / 2, buf, C_WHITE, LCD_TRANSPARENT, 1);
		} else {
			fmt_uptime(s->uptime, buf, sizeof(buf));
			row_text(l, sp, y, h, wd->size, "Uptime", buf, C_WHITE, 0, NULL, 0, &c->row_step);
		}
		break;
	case W_RULE:
		lcd_fill(l, sp.x0, y + h / 2 - 1, sp.x1 - sp.x0 + 1, 2, C_RULE);
		break;
	case W_SPEED:
		draw_speed_line(l, sp, y, h / 2, wd->size, 1, c->tr->down, c->tr->valid);
		draw_speed_line(l, sp, y + h / 2, h / 2, wd->size, 0, c->tr->up, c->tr->valid);
		break;
	case W_SPEED1: draw_speed1(l, sp, y, h, c->tr); break;
	case W_SPEEDBAR: draw_share_bar(l, c->tr, 0, sp.x0, y + (h - wd->size) / 2, sp.x1 - sp.x0 + 1, wd->size); break;
	case W_SPEEDDN: draw_share_bar_m(l, c->tr, BM_DRATE, sp.x0, y + (h - wd->size) / 2, sp.x1 - sp.x0 + 1, wd->size, 1, wd->showval); break;
	case W_SPEEDUP: draw_share_bar_m(l, c->tr, BM_URATE, sp.x0, y + (h - wd->size) / 2, sp.x1 - sp.x0 + 1, wd->size, 0, wd->showval); break;
	case W_USAGEBAR: draw_share_bar(l, c->tr, 1, sp.x0, y + (h - wd->size) / 2, sp.x1 - sp.x0 + 1, wd->size); break;
	case W_USAGEDN: draw_total_bar(l, c->tr, 1, sp.x0, y + (h - wd->size) / 2, sp.x1 - sp.x0 + 1, wd->size); break;
	case W_USAGEUP: draw_total_bar(l, c->tr, 0, sp.x0, y + (h - wd->size) / 2, sp.x1 - sp.x0 + 1, wd->size); break;
	case W_USAGE:
		if (c->tr->valid) fmt_bytes(c->tr->dtot + c->tr->utot, buf, sizeof(buf));
		else copy(buf, sizeof(buf), "--");
		row_text(l, sp, y, h, wd->size, "Total", buf, C_WHITE, 0, NULL, 0, &c->row_step);
		break;
	case W_LEGEND: draw_legend(l, w, c->tr, 0, wd->size, y, h, c->name_len, wd->bg, wd->fit, wd->peak, wd->split); break;
	case W_LEGENDTOTAL: draw_legend(l, w, c->tr, 1, wd->size, y, h, c->name_len, wd->bg, wd->fit, wd->peak, wd->split); break;
	case W_DONUT: draw_donut(l, c->tr, (sp.x0 + sp.x1 + 1) / 2, y + h / 2, (wd->size - 2) / 2); break;
	case W_MULTIWAN: draw_multiwan(l, w, s, c->tr, wd->size, y); break;
	case W_UPLINKS: draw_uplinks(l, sp, y, wd->size, s, c->tr, wd->layout, wd->generic, wd->noname, wd->noicon); break;
	case W_CPU:
		if (wd->cores == 2 && c->res->ncores > 0) draw_core_rows(l, sp, y, h, c->row_step ? 12 : wd->size, c->res);
		else if (wd->cores == 1 && c->res->ncores > 0) draw_core_bar(l, sp, y, h, c->row_step ? 12 : wd->size, c->res);
		else draw_resource(l, sp, y, h, c->row_step ? 12 : wd->size, "CPU", c->res->cpu, C_TRACK);
		break;
	case W_LOAD: {
		/* Background bar behind the row (like the legend's), width = load vs all cores;
		 * "LA" and the 1/5/15 triple (16 px) or 1-minute value (12 px) on top of it. */
		int nc = c->res->ncores > 0 ? c->res->ncores : 4, pct = c->res->load[0] / nc;
		char val[48];
		const font_t *lf = &FONT_T12, *vf = &FONT_T12;
		if (wd->size >= 16) snprintf(val, sizeof(val), "%d.%02d %d.%02d %d.%02d", c->res->load[0] / 100, c->res->load[0] % 100, c->res->load[1] / 100, c->res->load[1] % 100, c->res->load[2] / 100, c->res->load[2] % 100);
		else snprintf(val, sizeof(val), "%d.%02d", c->res->load[0] / 100, c->res->load[0] % 100);
		if (lcd_text_w(vf, val, 1) > sp.x1 - sp.x0 - 16) snprintf(val, sizeof(val), "%d.%02d", c->res->load[0] / 100, c->res->load[0] % 100);   /* no room for the triple */
		uint32_t col = pct < 60 ? C_OK : pct < 85 ? C_WARN : C_BAD;
		int bw = (sp.x1 + 1 - sp.x0) * (pct > 100 ? 100 : pct) / 100;
		if (bw > 0) lcd_fill(l, sp.x0, y + 1, bw, h - 2, dim_color(col, 34));
		lcd_fill(l, sp.x0, y + h - 3, bw, 1, col);          /* bright 1 px line at the bottom edge, as in the legend */
		lcd_draw(l, lf, sp.x0, y + (h - lf->h) / 2, "LA", C_DIM, LCD_TRANSPARENT, 1);
		lcd_draw(l, vf, sp.x1 + 1 - lcd_text_w(vf, val, 1), y + (h - vf->h) / 2, val, C_WHITE, LCD_TRANSPARENT, 1);
		break;
	}
	case W_RAM: draw_resource(l, sp, y, h, c->row_step ? 12 : wd->size, "RAM", c->res->ram, C_TRACK); break;
	case W_CPURAM: draw_cpuram(l, sp, y, h, c->row_step ? 12 : wd->size, c->res); break;
	case W_FLASH: draw_resource(l, sp, y, h, c->row_step ? 12 : wd->size, "Flash", c->res->flash, C_TRACK); break;
	case W_TEMP: {
		int big = wd->size >= 16 && !c->row_step;
		const font_t *lf = big ? &FONT_T16 : &FONT_T12, *vf = big ? &FONT_T16B : &FONT_T12;
		snprintf(buf, sizeof(buf), "%dC", c->res->temp);
		lcd_draw(l, lf, sp.x0, y + (h - lf->h) / 2, "Wi-Fi", C_DIM, LCD_TRANSPARENT, 1);
		lcd_draw(l, vf, sp.x1 + 1 - lcd_text_w(vf, buf, 1), y + (h - vf->h) / 2, buf, c->res->temp >= 85 ? C_BAD : c->res->temp >= 75 ? C_WARN : C_WHITE, LCD_TRANSPARENT, 1);
		break;
	}
	case W_STATUS: {
		/* Globe: green online, amber with "!" no internet, red slashed no uplink.
		 * With a full line it also carries the words; in a shared line the icon alone changes. */
		int ready = !strcmp(c->state, "ready"), noint = !strcmp(c->state, "noInternet");
		uint32_t col = ready ? C_OK : noint ? C_WARN : C_BAD;
		const char *text = ready ? "ONLINE" : noint ? "NO INTERNET" : "NO UPLINK";
		int r = wd->size / 2, cy = y + h / 2, cx = (sp.x0 + sp.x1 + 1) / 2;
		if (sp.x1 - sp.x0 + 1 >= 90) {
			int tw = lcd_text_w(&FONT_T16B, text, 1), x0 = cx - (wd->size + 6 + tw) / 2;
			cx = x0 + r;
			lcd_draw(l, &FONT_T16B, x0 + wd->size + 6, cy - FONT_T16B.h / 2, text, col, LCD_TRANSPARENT, 1);
		}
		draw_globe(l, cx, cy, r - 1, col);
		if (!ready && noint && sp.x1 - sp.x0 + 1 < 90) {          /* "!" cut into the globe */
			lcd_fill(l, cx - 1, cy - 4, 3, 6, C_BG);
			lcd_fill(l, cx - 1, cy + 3, 3, 3, C_BG);
		}
		break;
	}
	}
}

static void widget_draw(lcd_t *l, const win_t *w, const widget_t *wd, ctx_t *c, int y, int h)
{
	widget_draw_sp(l, w, wd, c, y, h, span_for(w, y, h));
}

/* ----------------------------------------------------------------------------------------- screen */

static void draw_screen(lcd_t *l, const win_t *w, const config_t *cf, const screen_t *sc, const status_t *s, const traffic_t *tr,
			const res_t *res, const char *state)
{
	ctx_t c = { s, tr, res, state, sc->backfill, 0, cf->name_len };
	lcd_fill(l, 0, 0, l->w, l->h, C_BG);

	if (!s->valid) {
		draw_pill(l, w, 6, "NO DATA", C_BAD);
		lcd_flush(l);
		return;
	}

	int up = !strcmp(s->up_state, "up");
	if (sc->backfill && up) {
		/* Wi-Fi uplinks only; with several, the screen is split into one vertical strip each */
		int wf[3], nw = 0;
		for (int i = 0; i < s->nups; i++)
			if (!strcmp(s->ups[i].type, "wifi") && s->ups[i].has_signal) wf[nw++] = s->ups[i].signal;
		if (!s->nups && !strcmp(s->up_type, "wifi") && s->has_signal) wf[nw++] = s->signal;
		for (int i = 0; i < nw; i++)
			draw_backfill(l, sig_pct(wf[i]), cf->fill_dim, cf->fill_edge, l->w * i / nw, l->w * (i + 1) / nw);
	}

	/* No panel indicator any more: the online status is a widget (status) placed like any other,
	 * so it can also share a line with e.g. the client count. Content starts at the top margin. */
	int top = w->y0 + 2;

	/* widgets: stacked, spare room spread evenly (at most 14 px between them) */
	int hs[MAX_WIDGETS], nvis = 0, total = 0, bottom = w->y1 - 3;
	for (int i = 0; i < sc->nw; i++) {
		hs[i] = widget_h(&sc->w[i], &c);
		if (hs[i]) { nvis++; total += hs[i]; }
	}
	for (int i = 0; i < sc->nw; i++) {        /* measure: all rows of a screen share one text size */
		int t = sc->w[i].type;
		if (hs[i] && (t == W_WANIP || t == W_LANIP || t == W_VPN || t == W_UPTIME || t == W_USAGE)) widget_draw(NULL, w, &sc->w[i], &c, top, hs[i]);
		if (hs[i] && t == W_ROW)              /* row members with row text measure the same way */
			for (int j = 0; j < sc->w[i].nsub; j++) {
				int st = sc->w[i].sub[j].type;
				if (st == W_WANIP || st == W_LANIP || st == W_VPN || st == W_UPTIME || st == W_USAGE) {
					widget_t mt = { .type = st, .size = sc->w[i].sub[j].size, .wrap = sc->w[i].sub[j].wrap, .shrink = sc->w[i].sub[j].shrink, .bg = sc->w[i].sub[j].bg, .fit = sc->w[i].sub[j].fit, .cores = sc->w[i].sub[j].cores, .layout = sc->w[i].sub[j].layout, .generic = sc->w[i].sub[j].generic, .noname = sc->w[i].sub[j].noname, .noicon = sc->w[i].sub[j].noicon, .showval = sc->w[i].sub[j].showval, .peak = sc->w[i].sub[j].peak, .split = sc->w[i].sub[j].split };
					widget_draw(NULL, w, &mt, &c, top, hs[i]);
				}
			}
	}
	if (nvis) {
		int spare = bottom - top - total, gap;
		if (sc->spacing >= 0) {          /* fixed gap, the stack stays vertically centred */
			gap = sc->spacing;
			if (nvis > 1 && gap * (nvis - 1) > spare) gap = spare > 0 ? spare / (nvis - 1) : 0;
		} else {                         /* auto: spread the spare room, at most 14 px between widgets */
			gap = nvis > 1 ? spare / (nvis + 1) : spare / 2;
			if (gap < 2) gap = 2;
			if (gap > 14) gap = 14;
		}
		/* stacks start at the top (auto leaves one gap above); spare room collects at the bottom */
		int y = sc->spacing >= 0 ? top : top + gap;
		if (y < top) y = top;
		for (int i = 0; i < sc->nw; i++) {
			if (!hs[i]) continue;
			widget_draw(l, w, &sc->w[i], &c, y, hs[i]);
			y += hs[i] + gap;
		}
	}
	if (sc->debug) {          /* test aid: the maxima the legend/bar scaling divides by */
		char dbg[64], a[16], b[16];
		double mxr = 0, mxt = 0;
		for (int i = 0; i < tr->n; i++) {
			if (tr->c[i].rate > mxr) mxr = tr->c[i].rate;
			if (tr->c[i].total > mxt) mxt = tr->c[i].total;
		}
		fmt_short(mxr, a, sizeof(a));
		fmt_short(mxt, b, sizeof(b));
		snprintf(dbg, sizeof(dbg), "R%s T%s n%d%s", a, b, tr->n, tr->valid ? "" : " STALE");
		span_t dsp = span_for(w, w->y1 - 14, 13);          /* inside the bezel window, unlike the panel edge */
		lcd_fill(l, dsp.x0, w->y1 - 14, dsp.x1 + 1 - dsp.x0, 14, RGB(0, 0, 0));
		lcd_draw(l, &FONT_T12, dsp.x0 + 2, w->y1 - 12, dbg, C_WARN, LCD_TRANSPARENT, 1);
	}
	lcd_flush(l);
}

/* Print the picture as the viewer sees it, as run-length text for the LuCI preview:
 * "W H" then "count:rrggbb" runs in row-major order, all on one line each. Bezel-hidden pixels
 * are the sentinel ff00ff, which consumers (LuCI canvas, rle2png.py) render transparent, so the
 * rounded panel sits directly on the page background with no frame at all. */
static void write_rle(lcd_t *l, const win_t *w)
{
	printf("%d %d\n", l->w, l->h);
	uint32_t cur = 0;
	int run = 0;
	for (int y = 0; y < l->h; y++)
		for (int x = 0; x < l->w; x++) {
			int px = l->flip_x ? l->phys_w - 1 - x : x, py = l->flip_y ? l->phys_h - 1 - y : y;
			uint32_t c = l->px[py * l->pitch + px];
			int in = x >= w->x0 && x <= w->x1 && y >= w->y0 && y <= w->y1;
			if (in) {                              /* rounded corners */
				int ci = corner_inset(w, y);
				if (x < w->x0 + ci || x > w->x1 - ci) in = 0;
			}
			if (!in) c = RGB(255, 0, 255);
			if (run && c == cur) { run++; continue; }
			if (run) printf("%d:%06x ", run, (unsigned)cur);
			cur = c; run = 1;
		}
	if (run) printf("%d:%06x", run, (unsigned)cur);
	printf("\n");
}

static int type_uses_traffic(int t) { return (t >= W_SPEED && t <= W_DONUT) || t == W_MULTIWAN || t == W_UPLINKS; }
static int type_uses_res(int t) { return t >= W_CPU && t <= W_CPURAM; }

static int screen_uses_traffic(const screen_t *sc)
{
	for (int i = 0; i < sc->nw; i++) {
		if (type_uses_traffic(sc->w[i].type)) return 1;
		if (sc->w[i].type == W_ROW)
			for (int j = 0; j < sc->w[i].nsub; j++) if (type_uses_traffic(sc->w[i].sub[j].type)) return 1;
	}
	return 0;
}

static int screen_uses_res(const screen_t *sc)
{
	for (int i = 0; i < sc->nw; i++) {
		if (type_uses_res(sc->w[i].type)) return 1;
		if (sc->w[i].type == W_ROW)
			for (int j = 0; j < sc->w[i].nsub; j++) if (type_uses_res(sc->w[i].sub[j].type)) return 1;
	}
	return 0;
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
	const char *cmd = "/usr/libexec/utr-lcd/collect.sh", *conf = "/var/run/utr-lcd/screens.conf", *tfile = "/var/run/utr-lcd/traffic";
	int interval = 5, rotate = 1, once = 0, once_screen = 1, list = 0, headless = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc) cmd = argv[++i];
		else if (!strcmp(argv[i], "-C") && i + 1 < argc) conf = argv[++i];
		else if (!strcmp(argv[i], "-T") && i + 1 < argc) tfile = argv[++i];
		else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-N") && i + 1 < argc) once_screen = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-0")) rotate = 0;
		else if (!strcmp(argv[i], "-1")) once = 1;
		else if (!strcmp(argv[i], "-l")) list = 1;
		else if (!strcmp(argv[i], "-P")) headless = once = 1;
		else {
			fprintf(stderr, "usage: %s [-c collector] [-C screens.conf] [-T traffic] [-i sec] [-0] [-1 [-N screen]] [-P] [-l]\n", argv[0]);
			return 2;
		}
	}
	if (interval < 1) interval = 1;

	config_t cf;
	conf_load(&cf, conf);
	/* The config file is rewritten (atomically) on every service reload, but procd does not
	 * restart this process for it - the command line does not change - so watch the file and
	 * pick up new layouts live. */
	struct stat cst;
	long long conf_key = stat(conf, &cst) == 0 ? (long long)cst.st_mtime * 1000000000LL + cst.st_mtim.tv_nsec : -1;
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
	if (headless ? lcd_open_mem(&l, rotate, rotate) : lcd_open(&l, "/dev/dri/card0", rotate, rotate)) return 1;

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
	traffic_t tr = { 0 }, tr_new;
	res_t res = { .temp = -1 };
	cpu_prev_t cpu_prev = { 0 };
	long res_at = 0;
	long tr_at = 0;
	unsigned tr_gen = 0;

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
			if (once && fails) fails = 2;           /* single shot (previews): one failed probe is enough */
		}
		if (!have_data && !coll.p && now >= next_collect + 10000) next_collect = now;   /* collector failed to start */

		if (now - bl_checked >= 500) {
			dark = backlight_off(); bl_checked = now;
			if (!once && stat(conf, &cst) == 0) {
				long long k = (long long)cst.st_mtime * 1000000000LL + cst.st_mtim.tv_nsec;
				if (k != conf_key) {
					config_t nc;
					conf_load(&nc, conf);
					cf = nc; conf_key = k;
					norder = build_order(&cf, order); pos = 0; drawn_pos = -1; prev[0] = 0; switch_at = now;
					if (trace) fprintf(stderr, "%ld config reloaded, %d screens\n", now, cf.ns);
				}
			}
		}

		/* screen rotation */
		if (drawn_pos < 0 || dark) switch_at = now + cf.s[order[pos]].dwell_ms;
		else if (norder > 1 && now >= switch_at) {
			pos = (pos + 1) % norder;
			switch_at = now + cf.s[order[pos]].dwell_ms;
		}

		/* traffic data is only read while the screen on show uses it */
		if (screen_uses_traffic(&cf.s[order[pos]]) && (now - tr_at >= 1000 || tr_at == 0 || pos != drawn_pos)) {
			traffic_load(&tr_new, tfile);
			if (memcmp(&tr, &tr_new, sizeof(tr))) { tr = tr_new; tr_gen++; }
			tr_at = now;
		}
		/* resources: read once a second while the screen on show has such rows (CPU load needs two samples) */
		if (screen_uses_res(&cf.s[order[pos]]) && (now - res_at >= 1000 || res_at == 0)) {
			res_sample(&res, &cpu_prev);
			res_at = now;
		}
		if (have_data) {
			int up = !strcmp(st.up_state, "up");
			const char *state = !up ? "noUplink" : fails >= 2 ? "noInternet" : "ready";
			char up_s[32], ups_s[160] = "";
			fmt_uptime(st.uptime, up_s, sizeof(up_s));
			for (int i = 0; i < st.nups; i++)
				snprintf(ups_s + strlen(ups_s), sizeof(ups_s) - strlen(ups_s), "%s/%s/%d,", st.ups[i].dev, st.ups[i].name, st.ups[i].signal);
			for (int i = 0; i < st.nmw; i++)
				snprintf(ups_s + strlen(ups_s), sizeof(ups_s) - strlen(ups_s), "mw%s/%s/%d,", st.mw[i].ifname, st.mw[i].state, st.mw[i].pct);
			snprintf(sig, sizeof(sig), "%d,%d,%d,%d,%d,%d,%d,%d,%d|%u|%d|%s|%d|%s|%s|%s|%s|%s|%d|%d|%d|%s|%s|%s|%d|%s", res.cpu, res.ram, res.flash, res.temp, res.load[0], res.load[1], res.load[2], res.core[0], res.core[1], tr_gen, pos, state, st.valid, st.up_type,
				 st.up_name, st.up_detail, st.wan_ip, st.lan_ip, st.has_signal, st.signal, st.speed, st.vpn_state,
				 st.vpn_name, st.inet, st.clients, up_s);
			strncat(sig, ups_s, sizeof(sig) - strlen(sig) - 1);
			if (pos != drawn_pos || strcmp(sig, prev)) {
				draw_screen(&l, &w, &cf, &cf.s[order[pos]], &st, &tr, &res, state);
				if (trace) fprintf(stderr, "%ld screen %d\n", now, order[pos] + 1);
				drawn_pos = pos;
				copy(prev, sizeof(prev), sig);
			}
			if (once && have_data) {
				if (headless) write_rle(&l, &w);
				else sleep(LCD_ONCE_HOLD);
				break;
			}
		}

		long wait = 100;
		if (norder > 1 && !dark && switch_at - now_ms() < wait) wait = switch_at - now_ms() > 1 ? switch_at - now_ms() : 1;
		if (coll.p) { struct pollfd pf = { .fd = coll.fd, .events = POLLIN }; poll(&pf, 1, (int)wait); }
		else { struct timespec ts = { 0, wait * 1000000L }; nanosleep(&ts, NULL); }
	}

	if (!headless && (!once || LCD_ONCE_HOLD > 0)) {      /* blank the panel on exit (not for zero-hold previews) */
		lcd_fill(&l, 0, 0, l.w, l.h, LCD_BLACK);
		lcd_flush(&l);
	}
	lcd_close(&l);
	return 0;
}
