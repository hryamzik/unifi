#!/bin/sh
# Run the stock Ubiquiti LCD UI (ulcmd) from the optional stock bundle.
#
# The stock binaries are soft-float ARM musl executables; OpenWrt here is hard-float, so they are
# started through the bundle's own musl loader and libraries. utr-lcd-fbshim.so (LD_PRELOAD) fakes
# /dev/fb0 as a file under $UTR_RUN and refuses system()/popen(), so the UI cannot run poweroff
# or anything else on the router.
STOCK=${UTR_STOCK:-/usr/share/utr-lcd/stock}
RUN=${UTR_RUN:-/var/run/utr-lcd}
L=$STOCK/lib

mkdir -p "$RUN"
export FBSHIM_FILE=$RUN/fb0.raw FBSHIM_WIRELESS=$RUN/wireless FBSHIM_LOG=$RUN/shim.log
export LD_PRELOAD=/usr/lib/utr-lcd/utr-lcd-fbshim.so

# wait for the private ubus daemon
i=0
while [ ! -S /var/run/ubus.sock ] && [ "$i" -lt 20 ]; do sleep 1; i=$((i + 1)); done

exec "$L/libc.so" --library-path "$L" "$STOCK/bin/ulcmd" -c
