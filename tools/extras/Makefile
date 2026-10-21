# Small hardware test programs (colour bars, orientation, safe-area frames).
#   make CC=<target gcc>   then copy to the router and run, e.g.  ./lcdorient -x -y
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -Wall -I../../package/utr-lcd/src -I/usr/include/libdrm
all: lcdtest lcdorient lcdframes lcdgrid lcdfill
%: %.c ../../package/utr-lcd/src/lcd.h
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f lcdtest lcdorient lcdframes lcdgrid lcdfill
