# Standalone build; the OpenWrt package Makefile calls this with the target toolchain.
#   make                       native build (needs DRM UAPI or libdrm headers)
#   make CC=arm-openwrt-linux-muslgnueabi-gcc
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -std=gnu11 -I/usr/include/libdrm   # libdrm path is only a fallback for non-OpenWrt hosts
PROGS    = utr-lcd-status utr-lcd-fb2drm
SHIM     = utr-lcd-fbshim.so

all: $(PROGS) $(SHIM)

utr-lcd-status: lcd-status.c lcd.h fonts.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ lcd-status.c -lm

utr-lcd-fb2drm: fb2drm.c lcd.h fonts.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ fb2drm.c

$(SHIM): fbshim.c
	$(CC) $(CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ fbshim.c

clean:
	rm -f $(PROGS) $(SHIM)
.PHONY: all clean
