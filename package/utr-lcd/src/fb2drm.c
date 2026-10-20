/*
 * fb2drm - copy the stock UI's fake framebuffer (135x240 RGB565, see fbshim.c) to the real
 * LCD through DRM. Applies the UTR-LR's 180 degree panel rotation and an optional offset.
 *
 * Usage: fb2drm [-f fbfile] [-x offset_x] [-y offset_y] [-i interval_ms] [-0]
 *   -f  framebuffer file (default /var/run/utr-lcd/fb0.raw)
 *   -x/-y  place the 135x240 image at this offset inside the 136x240 panel (default 0,0)
 *   -0  do not rotate 180 degrees
 */
#include <signal.h>
#include <time.h>
#include "lcd.h"

#define FB_W 135
#define FB_H 240
#define FB_BYTES (FB_W * FB_H * 2)

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	const char *file = "/var/run/utr-lcd/fb0.raw";
	int ox = 0, oy = 0, interval = 100, rotate = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-f") && i + 1 < argc) file = argv[++i];
		else if (!strcmp(argv[i], "-x") && i + 1 < argc) ox = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-y") && i + 1 < argc) oy = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-0")) rotate = 0;
		else { fprintf(stderr, "usage: %s [-f file] [-x N] [-y N] [-i ms] [-0]\n", argv[0]); return 2; }
	}
	if (interval < 20) interval = 20;

	struct sigaction sa = { .sa_handler = on_sig };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	lcd_t l;
	if (lcd_open(&l, "/dev/dri/card0", rotate, rotate)) return 1;
	lcd_fill(&l, 0, 0, l.w, l.h, LCD_BLACK);
	lcd_flush(&l);

	static unsigned char cur[FB_BYTES], prev[FB_BYTES];
	int have_prev = 0;
	struct timespec ts = { .tv_sec = interval / 1000, .tv_nsec = (interval % 1000) * 1000000L };

	while (!stop) {
		int fd = open(file, O_RDONLY);
		ssize_t n = fd >= 0 ? read(fd, cur, FB_BYTES) : -1;
		if (fd >= 0) close(fd);
		if (n == FB_BYTES && (!have_prev || memcmp(cur, prev, FB_BYTES))) {
			for (int y = 0; y < FB_H; y++)
				for (int x = 0; x < FB_W; x++) {
					unsigned v = cur[(y * FB_W + x) * 2] | (cur[(y * FB_W + x) * 2 + 1] << 8);
					unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
					lcd_pixel(&l, x + ox, y + oy, RGB(r * 255 / 31, g * 255 / 63, b * 255 / 31));
				}
			lcd_flush(&l);
			memcpy(prev, cur, FB_BYTES);
			have_prev = 1;
		}
		nanosleep(&ts, NULL);
	}

	lcd_fill(&l, 0, 0, l.w, l.h, LCD_BLACK);
	lcd_flush(&l);
	lcd_close(&l);
	return 0;
}
