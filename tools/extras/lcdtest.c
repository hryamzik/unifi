/*
 * lcdtest - minimal DRM dumb-buffer test for the UTR-LR LCD (panel-mipi-dbi).
 * Draws colour bars + orientation markers, holds them, then exits.
 * Uses raw DRM ioctls (no libdrm) so it builds as a small static binary.
 *
 * Usage: lcdtest [/dev/dri/card0] [hold_seconds]
 */
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
#if defined(__has_include) && __has_include(<drm/drm.h>)
#include <drm/drm.h>
#include <drm/drm_mode.h>
#else
#include <drm.h>
#include <drm_mode.h>
#endif

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static void fill(uint32_t *px, int stride, int x0, int y0, int w, int h, uint32_t c, int W, int H)
{
	for (int y = y0; y < y0 + h && y < H; y++)
		for (int x = x0; x < x0 + w && x < W; x++)
			px[y * stride + x] = c;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
	int hold = argc > 2 ? atoi(argv[2]) : 10;

	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }

	/* --- resources: first call for counts, second for ids --- */
	struct drm_mode_card_res res = {0};
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getresources"); return 1; }
	if (!res.count_connectors || !res.count_crtcs) { fprintf(stderr, "no connectors/crtcs\n"); return 1; }
	uint32_t conn_ids[8] = {0}, crtc_ids[8] = {0}, enc_ids[8] = {0}, fb_ids[8] = {0};
	if (res.count_connectors > 8) res.count_connectors = 8;
	if (res.count_crtcs > 8) res.count_crtcs = 8;
	res.connector_id_ptr = (uintptr_t)conn_ids;
	res.crtc_id_ptr = (uintptr_t)crtc_ids;
	res.encoder_id_ptr = (uintptr_t)enc_ids;
	res.fb_id_ptr = (uintptr_t)fb_ids;
	if (res.count_encoders > 8) res.count_encoders = 8;
	if (res.count_fbs > 8) res.count_fbs = 8;
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getresources2"); return 1; }
	printf("connectors=%u crtcs=%u encoders=%u\n", res.count_connectors, res.count_crtcs, res.count_encoders);

	/* --- connector: counts first, then modes/encoders --- */
	uint32_t conn_id = conn_ids[0];
	struct drm_mode_get_connector gc = { .connector_id = conn_id };
	if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) { perror("getconnector"); return 1; }
	/* The prop/enc/mode counts can change between calls; retry a few times. */
	struct drm_mode_modeinfo modes[16] = {0};
	uint32_t cenc[8] = {0};
	gc.count_props = 0;
	if (gc.count_modes > 16) gc.count_modes = 16;
	if (gc.count_encoders > 8) gc.count_encoders = 8;
	gc.modes_ptr = (uintptr_t)modes;
	gc.encoders_ptr = (uintptr_t)cenc;
	gc.props_ptr = 0;
	gc.prop_values_ptr = 0;
	if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) { perror("getconnector2"); return 1; }
	printf("connector %u type=%u connection=%u modes=%u encoders=%u\n",
	       conn_id, gc.connector_type, gc.connection, gc.count_modes, gc.count_encoders);
	if (!gc.count_modes) { fprintf(stderr, "connector has no modes\n"); return 1; }

	struct drm_mode_modeinfo mode = modes[0];
	printf("mode %s %ux%u@%u\n", mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh);

	/* --- pick a CRTC via the encoder --- */
	uint32_t crtc_id = crtc_ids[0];
	uint32_t enc_id = gc.encoder_id ? gc.encoder_id : (gc.count_encoders ? cenc[0] : 0);
	if (enc_id) {
		struct drm_mode_get_encoder ge = { .encoder_id = enc_id };
		if (!xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge))
			for (uint32_t i = 0; i < res.count_crtcs; i++)
				if (ge.possible_crtcs & (1u << i)) { crtc_id = crtc_ids[i]; break; }
	}

	/* --- dumb buffer, XRGB8888 (driver converts to RGB565 for the panel) --- */
	struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return 1; }

	struct drm_mode_fb_cmd fbc = {
		.width = cd.width, .height = cd.height, .pitch = cd.pitch,
		.bpp = 32, .depth = 24, .handle = cd.handle,
	};
	if (xioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc)) { perror("addfb"); return 1; }

	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map_dumb"); return 1; }
	uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (px == MAP_FAILED) { perror("mmap"); return 1; }

	int W = mode.hdisplay, H = mode.vdisplay, S = cd.pitch / 4;

	/* colour bars (top 3/4) */
	static const uint32_t bars[] = {
		0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
	};
	int bh = H * 3 / 4, nb = sizeof(bars) / sizeof(bars[0]);
	for (int y = 0; y < bh; y++)
		for (int x = 0; x < W; x++)
			px[y * S + x] = bars[y * nb / bh];

	/* grayscale ramp (bottom 1/4) */
	for (int y = bh; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint32_t g = x * 255 / (W - 1);
			px[y * S + x] = (g << 16) | (g << 8) | g;
		}

	/* orientation markers: TL red, TR green, BL blue, BR white (12px) */
	fill(px, S, 0, 0, 12, 12, 0xFF0000, W, H);
	fill(px, S, W - 12, 0, 12, 12, 0x00FF00, W, H);
	fill(px, S, 0, H - 12, 12, 12, 0x0000FF, W, H);
	fill(px, S, W - 12, H - 12, 12, 12, 0xFFFFFF, W, H);

	/* 1px frame to expose clipping/offset */
	for (int x = 0; x < W; x++) px[x] = px[(H - 1) * S + x] = 0xFFFFFF;
	for (int y = 0; y < H; y++) px[y * S] = px[y * S + W - 1] = 0xFFFFFF;

	/* --- modeset --- */
	struct drm_mode_crtc sc = {
		.crtc_id = crtc_id, .fb_id = fbc.fb_id,
		.set_connectors_ptr = (uintptr_t)&conn_id, .count_connectors = 1,
		.mode_valid = 1, .mode = mode,
	};
	if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) { perror("setcrtc"); return 1; }

	struct drm_mode_fb_dirty_cmd dirty = { .fb_id = fbc.fb_id };
	xioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty); /* flush; ignore if unsupported */

	printf("drawn; holding %d s\n", hold);
	sleep(hold);

	munmap(px, cd.size);
	return 0;
}
