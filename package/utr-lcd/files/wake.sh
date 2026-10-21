#!/bin/sh
# utr-lcd raise-to-wake: wake the stock UI's screen when the router is moved. Polls the on-board
# LIS2DW12 accelerometer (kernel IIO sysfs, read-only) and calls `ui userActivity` when the reading
# changes by more than a threshold. BusyBox ash.
#
#   UTR_LIBEXEC   helper dir (default /usr/libexec/utr-lcd)
#   UTR_WAKE_THR  raw units, ~1000 per g (default 80, about 0.08 g)
IIO=${UTR_IIO:-/sys/bus/iio/devices/iio:device0}
LIBEXEC=${UTR_LIBEXEC:-/usr/libexec/utr-lcd}
THR=${UTR_WAKE_THR:-80}
CALL=$LIBEXEC/ui-call.sh

[ -r "$IIO/in_accel_x_raw" ] || { logger -t utr-lcd "wake: no accelerometer at $IIO"; exit 0; }

abs() { [ "$1" -lt 0 ] && echo $((0 - $1)) || echo "$1"; }

px="" py="" pz=""
while :; do
	x=$(cat "$IIO/in_accel_x_raw" 2>/dev/null) y=$(cat "$IIO/in_accel_y_raw" 2>/dev/null) z=$(cat "$IIO/in_accel_z_raw" 2>/dev/null)
	case "$x$y$z" in ''|*[!0-9-]*) sleep 1; continue ;; esac
	if [ -n "$px" ]; then
		dx=$(abs $((x - px))) dy=$(abs $((y - py))) dz=$(abs $((z - pz)))
		if [ "$dx" -gt "$THR" ] || [ "$dy" -gt "$THR" ] || [ "$dz" -gt "$THR" ]; then
			$CALL userActivity '{"value":true}' >/dev/null 2>&1
		fi
	fi
	px=$x py=$y pz=$z
	sleep 1
done
