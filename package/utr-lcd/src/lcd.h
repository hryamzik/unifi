/*
 * lcd.h - tiny DRM dumb-buffer display layer for the UTR-LR LCD (136x240 ST7789-class panel
 * driven by panel-mipi-dbi). Raw DRM ioctls, no libdrm. Header-only.
 *
 * Orientation is handled in software (flip_x / flip_y / swap), so no firmware or DT change
 * is needed to correct how the image sits on the glass.
 */
#ifndef LCD_H
#define LCD_H

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef LCD_MOCK
#if defined(__has_include) && __has_include(<drm/drm.h>)
#include <drm/drm.h>        /* kernel UAPI headers (OpenWrt toolchain) */
#include <drm/drm_mode.h>
#else
#include <drm.h>            /* libdrm's copy (-I/usr/include/libdrm) */
#include <drm_mode.h>
#endif
#endif /* !LCD_MOCK */

#include "fonts.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define LCD_BLACK RGB(0, 0, 0)
#define LCD_WHITE RGB(255, 255, 255)

typedef struct {
	int fd;
	uint32_t *px;          /* mapped XRGB8888 buffer */
	int phys_w, phys_h;    /* panel pixels, as scanned */
	int pitch;             /* pixels per row */
	size_t size;
	uint32_t fb, crtc, conn, handle;
	/* logical view */
	int flip_x, flip_y;    /* mirror logical coords onto the glass */
	int w, h;              /* logical size (== phys for now) */
	int dirty_warned;
	int mem_only;          /* no display: draw into memory only (previews) */
} lcd_t;

#ifndef LCD_MOCK
static inline int lcd__ioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

/* Returns 0 on success, -1 on error (message on stderr). */
static int lcd_open(lcd_t *l, const char *dev, int flip_x, int flip_y)
{
	memset(l, 0, sizeof(*l));
	l->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (l->fd < 0) { perror("lcd: open"); return -1; }

	struct drm_mode_card_res res = {0};
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("lcd: getresources"); return -1; }
	if (!res.count_connectors || !res.count_crtcs) { fprintf(stderr, "lcd: no connector/crtc\n"); return -1; }
	uint32_t conn_ids[8] = {0}, crtc_ids[8] = {0}, enc_ids[8] = {0}, fb_ids[8] = {0};
	if (res.count_connectors > 8) res.count_connectors = 8;
	if (res.count_crtcs > 8) res.count_crtcs = 8;
	if (res.count_encoders > 8) res.count_encoders = 8;
	if (res.count_fbs > 8) res.count_fbs = 8;
	res.connector_id_ptr = (uintptr_t)conn_ids;
	res.crtc_id_ptr = (uintptr_t)crtc_ids;
	res.encoder_id_ptr = (uintptr_t)enc_ids;
	res.fb_id_ptr = (uintptr_t)fb_ids;
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("lcd: getresources2"); return -1; }

	l->conn = conn_ids[0];
	struct drm_mode_get_connector gc = { .connector_id = l->conn };
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) { perror("lcd: getconnector"); return -1; }
	struct drm_mode_modeinfo modes[16] = {0};
	uint32_t cenc[8] = {0};
	gc.count_props = 0;
	if (gc.count_modes > 16) gc.count_modes = 16;
	if (gc.count_encoders > 8) gc.count_encoders = 8;
	gc.modes_ptr = (uintptr_t)modes;
	gc.encoders_ptr = (uintptr_t)cenc;
	gc.props_ptr = gc.prop_values_ptr = 0;
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) { perror("lcd: getconnector2"); return -1; }
	if (!gc.count_modes) { fprintf(stderr, "lcd: connector has no modes\n"); return -1; }
	struct drm_mode_modeinfo mode = modes[0];

	l->crtc = crtc_ids[0];
	uint32_t enc_id = gc.encoder_id ? gc.encoder_id : (gc.count_encoders ? cenc[0] : 0);
	if (enc_id) {
		struct drm_mode_get_encoder ge = { .encoder_id = enc_id };
		if (!lcd__ioctl(l->fd, DRM_IOCTL_MODE_GETENCODER, &ge))
			for (uint32_t i = 0; i < res.count_crtcs; i++)
				if (ge.possible_crtcs & (1u << i)) { l->crtc = crtc_ids[i]; break; }
	}

	struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("lcd: create_dumb"); return -1; }
	l->handle = cd.handle;
	l->size = cd.size;

	struct drm_mode_fb_cmd fbc = {
		.width = cd.width, .height = cd.height, .pitch = cd.pitch,
		.bpp = 32, .depth = 24, .handle = cd.handle,
	};
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_ADDFB, &fbc)) { perror("lcd: addfb"); return -1; }
	l->fb = fbc.fb_id;

	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("lcd: map_dumb"); return -1; }
	l->px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, l->fd, md.offset);
	if (l->px == MAP_FAILED) { perror("lcd: mmap"); return -1; }

	l->phys_w = mode.hdisplay; l->phys_h = mode.vdisplay; l->pitch = cd.pitch / 4;
	l->w = l->phys_w; l->h = l->phys_h;
	l->flip_x = flip_x; l->flip_y = flip_y;
	memset(l->px, 0, l->size);

	struct drm_mode_crtc sc = {
		.crtc_id = l->crtc, .fb_id = l->fb,
		.set_connectors_ptr = (uintptr_t)&l->conn, .count_connectors = 1,
		.mode_valid = 1, .mode = mode,
	};
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_SETCRTC, &sc)) { perror("lcd: setcrtc"); return -1; }
	return 0;
}

#else  /* LCD_MOCK: no hardware; lcd_flush writes the raw (as scanned) panel image as PPM to $LCD_MOCK_OUT */
static int lcd_open(lcd_t *l, const char *dev, int flip_x, int flip_y)
{
	(void)dev;
	memset(l, 0, sizeof(*l));
	l->phys_w = 136; l->phys_h = 240; l->pitch = 136;
	l->size = (size_t)l->pitch * l->phys_h * 4;
	l->px = calloc(1, l->size);
	l->w = l->phys_w; l->h = l->phys_h;
	l->flip_x = flip_x; l->flip_y = flip_y;
	return l->px ? 0 : -1;
}

#endif

/* Headless: a 136x240 buffer in memory, nothing is sent anywhere (lcd_flush does nothing). */
static int lcd_open_mem(lcd_t *l, int flip_x, int flip_y)
{
	memset(l, 0, sizeof(*l));
	l->phys_w = 136; l->phys_h = 240; l->pitch = 136;
	l->size = (size_t)l->pitch * l->phys_h * 4;
	l->px = calloc(1, l->size);
	l->w = l->phys_w; l->h = l->phys_h;
	l->flip_x = flip_x; l->flip_y = flip_y;
	l->mem_only = 1;
	return l->px ? 0 : -1;
}

static inline void lcd_pixel(lcd_t *l, int x, int y, uint32_t c)
{
	if (x < 0 || y < 0 || x >= l->w || y >= l->h) return;
	if (l->flip_x) x = l->phys_w - 1 - x;
	if (l->flip_y) y = l->phys_h - 1 - y;
	l->px[y * l->pitch + x] = c;
}

static void lcd_fill(lcd_t *l, int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			lcd_pixel(l, x, y, c);
}

#define LCD_TRANSPARENT 0xFF000000u    /* as bg: leave the background untouched */

/* Draw one glyph of font f; scale>=1 magnifies it. bg may be LCD_TRANSPARENT. */
static void lcd_glyph(lcd_t *l, const font_t *f, int x, int y, char ch, uint32_t fg, uint32_t bg, int scale)
{
	if (ch < 32 || ch > 126) ch = '?';
	const unsigned char *g = f->data + (ch - 32) * f->h * f->stride;
	for (int row = 0; row < f->h; row++)
		for (int col = 0; col < f->w; col++) {
			int on = g[row * f->stride + col / 8] & (0x80 >> (col % 8));
			if (!on && bg == LCD_TRANSPARENT) continue;
			lcd_fill(l, x + col * scale, y + row * scale, scale, scale, on ? fg : bg);
		}
}

/* Width in pixels of s in font f. */
static inline int lcd_text_w(const font_t *f, const char *s, int scale)
{
	return (int)strlen(s) * f->w * scale;
}

/* Draw a string with font f; returns the x position after the last char. */
static int lcd_draw(lcd_t *l, const font_t *f, int x, int y, const char *s, uint32_t fg, uint32_t bg, int scale)
{
	for (; *s; s++, x += f->w * scale)
		lcd_glyph(l, f, x, y, *s, fg, bg, scale);
	return x;
}

/* Simple 8x16 text (kept for the test programs). */
static inline int lcd_text(lcd_t *l, int x, int y, const char *s, uint32_t fg, uint32_t bg, int scale)
{
	return lcd_draw(l, &FONT_T16, x, y, s, fg, bg, scale);
}

/* Filled rounded rectangle, corner radius r (pixel-centre test against the corner circle). */
static void lcd_rrect(lcd_t *l, int x, int y, int w, int h, int r, uint32_t c)
{
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	for (int row = 0; row < h; row++) {
		int rr = row < h - 1 - row ? row : h - 1 - row;   /* distance from nearest horizontal edge */
		int inset = 0;
		if (rr < r) {
			int t = 2 * (r - rr) - 1;
			while (inset < r && (2 * (r - inset) - 1) * (2 * (r - inset) - 1) + t * t > 4 * r * r) inset++;
		}
		lcd_fill(l, x + inset, y + row, w - 2 * inset, 1, c);
	}
}

/* Filled circle with centre (cx,cy) and radius r. */
static void lcd_disc(lcd_t *l, int cx, int cy, int r, uint32_t c)
{
	for (int dy = -r; dy <= r; dy++)
		for (int dx = -r; dx <= r; dx++)
			if (dx * dx + dy * dy <= r * r + r / 2) lcd_pixel(l, cx + dx, cy + dy, c);
}

#ifndef LCD_MOCK
/* Push the buffer to the panel. Warns once on failure. */
static void lcd_flush(lcd_t *l)
{
	if (l->mem_only) return;
	struct drm_mode_fb_dirty_cmd d = { .fb_id = l->fb };
	if (lcd__ioctl(l->fd, DRM_IOCTL_MODE_DIRTYFB, &d) && !l->dirty_warned) {
		perror("lcd: dirtyfb");
		l->dirty_warned = 1;
	}
}

static void lcd_close(lcd_t *l)
{
	if (l->mem_only) { free(l->px); return; }
	if (l->px && l->px != MAP_FAILED) munmap(l->px, l->size);
	if (l->fd > 0) close(l->fd);
}

#else
static void lcd_flush(lcd_t *l)
{
	if (l->mem_only) return;
	const char *out = getenv("LCD_MOCK_OUT");
	FILE *f = out ? fopen(out, "wb") : NULL;
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", l->phys_w, l->phys_h);
	for (int i = 0; i < l->phys_w * l->phys_h; i++) {
		uint32_t c = l->px[i];
		fputc(c >> 16, f); fputc(c >> 8, f); fputc(c, f);
	}
	fclose(f);
}

static void lcd_close(lcd_t *l) { free(l->px); }
#endif

#endif /* LCD_H */
