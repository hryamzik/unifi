/*
 * fbshim - LD_PRELOAD shim for running the stock Ubiquiti ulcmd UI outside the stock firmware.
 *
 *  - fakes /dev/fb0 as a 135x240 RGB565 buffer backed by $FBSHIM_FILE (default /tmp/fb0.raw)
 *  - redirects /proc/net/wireless to $FBSHIM_WIRELESS if that file exists (lets a feeder present
 *    a stock-style "ath4" interface with the real signal level)
 *  - redirects the UI's settings file /etc/ulcmd/ulcmd.conf to $FBSHIM_CONF (RAM), so the UI can
 *    save and apply its settings (theme, brightness, ...) without writing to the router's flash
 *  - SAFETY: refuses system()/popen(): logs the command to $FBSHIM_LOG and fails, so the UI can
 *    never run poweroff / reboot / anything else on the host.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fb.h>

#define W 135
#define H 240
#define BPP 16

static int fb_fd = -1;

static const char *fb_path(void) { const char *p = getenv("FBSHIM_FILE"); return p ? p : "/tmp/fb0.raw"; }

static void log_refused(const char *what, const char *cmd)
{
	const char *lp = getenv("FBSHIM_LOG");
	FILE *f = fopen(lp ? lp : "/tmp/fbshim.log", "a");
	if (f) { fprintf(f, "refused %s: %s\n", what, cmd ? cmd : "(null)"); fclose(f); }
}

/* Map the paths we care about; returns the path to really open. */
static const char *map_path(const char *path)
{
	static char buf[256];
	if (!path) return path;
	if (!strcmp(path, "/dev/fb0")) return fb_path();
	if (!strcmp(path, "/proc/net/wireless")) {
		const char *w = getenv("FBSHIM_WIRELESS");
		if (w && access(w, R_OK) == 0) { snprintf(buf, sizeof buf, "%s", w); return buf; }
	}
	if (!strcmp(path, "/etc/ulcmd/ulcmd.conf")) {
		const char *c = getenv("FBSHIM_CONF");
		if (c) { snprintf(buf, sizeof buf, "%s", c); return buf; }
	}
	return path;
}

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
	int is_fb = path && !strcmp(path, "/dev/fb0");
	int fd = syscall(SYS_open, map_path(path), is_fb ? (O_RDWR | O_CREAT) : flags, is_fb ? 0666 : mode);
	if (is_fb && fd >= 0) {
		syscall(SYS_ftruncate, fd, W * H * BPP / 8);
		fb_fd = fd;
		fprintf(stderr, "fbshim: /dev/fb0 -> %s (fd %d)\n", fb_path(), fd);
	}
	return fd;
}
int open64(const char *path, int flags, ...) __attribute__((alias("open")));

FILE *fopen(const char *path, const char *mode)
{
	/* use open() + fdopen() so we do not depend on dlsym */
	int fl = O_RDONLY;
	if (mode[0] == 'w') fl = O_WRONLY | O_CREAT | O_TRUNC;
	else if (mode[0] == 'a') fl = O_WRONLY | O_CREAT | O_APPEND;
	if (strchr(mode, '+')) fl = (fl & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
	int fd = syscall(SYS_open, map_path(path), fl, 0666);
	if (fd < 0) return NULL;
	FILE *f = fdopen(fd, mode);
	if (!f) close(fd);
	return f;
}
FILE *fopen64(const char *path, const char *mode) __attribute__((alias("fopen")));

int system(const char *cmd)
{
	if (!cmd) return 0;
	log_refused("system", cmd);
	return -1;
}

FILE *popen(const char *cmd, const char *type)
{
	(void)type;
	log_refused("popen", cmd);
	errno = ENOSYS;
	return NULL;
}

int ioctl(int fd, int req, ...)
{
	va_list ap; va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (fd == fb_fd && fd >= 0) {
		switch ((unsigned)req) {
		case FBIOGET_FSCREENINFO: {
			struct fb_fix_screeninfo *f = arg;
			memset(f, 0, sizeof(*f));
			strcpy(f->id, "st7789fb");
			f->smem_len = W * H * BPP / 8;
			f->type = FB_TYPE_PACKED_PIXELS;
			f->visual = FB_VISUAL_TRUECOLOR;
			f->line_length = W * BPP / 8;
			return 0;
		}
		case FBIOGET_VSCREENINFO: {
			struct fb_var_screeninfo *v = arg;
			memset(v, 0, sizeof(*v));
			v->xres = v->xres_virtual = W;
			v->yres = v->yres_virtual = H;
			v->bits_per_pixel = BPP;
			v->red.offset = 11;   v->red.length = 5;
			v->green.offset = 5;  v->green.length = 6;
			v->blue.offset = 0;   v->blue.length = 5;
			v->height = 25; v->width = 14;
			v->rotate = FB_ROTATE_UR; /* render upright; the bridge applies the panel rotation */
			return 0;
		}
		default:
			return 0; /* PUT_VSCREENINFO, BLANK, PAN_DISPLAY, ... */
		}
	}
	return syscall(SYS_ioctl, fd, req, arg);
}
