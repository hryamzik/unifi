#!/bin/sh
# utr-lcd power manager: dims and/or blanks the backlight after a period without motion and restores
# it when the router is moved. Works with both display modes (it only touches the backlight).
# Motion comes from the on-board LIS2DW12 accelerometer (kernel IIO sysfs, read-only). BusyBox ash.
#
#   UTR_FULL       backlight percent while active (default 100)
#   UTR_DIM_AFTER  seconds without motion before dimming, 0 = never dim (default 0)
#   UTR_DIM_LEVEL  backlight percent while dimmed (default 15)
#   UTR_OFF_AFTER  seconds without motion before the backlight goes off, 0 = never (default 0)
#   UTR_WAKE_THR   raw accelerometer units that count as motion, ~1000 per g (default 80)
#   UTR_RUN        scratch dir (default /var/run/utr-lcd)
#   UTR_IIO        accelerometer directory (default /sys/bus/iio/devices/iio:device0)
BL=${UTR_BACKLIGHT:-/sys/class/backlight/backlight}
IIO=${UTR_IIO:-/sys/bus/iio/devices/iio:device0}
RUN=${UTR_RUN:-/var/run/utr-lcd}

num() { case "$1" in ''|*[!0-9]*) echo "$2" ;; *) echo "$1" ;; esac; }
FULL=$(num "$UTR_FULL" 100); DIM_AFTER=$(num "$UTR_DIM_AFTER" 0); DIM_LEVEL=$(num "$UTR_DIM_LEVEL" 15)
OFF_AFTER=$(num "$UTR_OFF_AFTER" 0); THR=$(num "$UTR_WAKE_THR" 80)
[ "$FULL" -lt 1 ] && FULL=1; [ "$FULL" -gt 100 ] && FULL=100
[ "$DIM_LEVEL" -gt "$FULL" ] && DIM_LEVEL=$FULL

[ -w "$BL/brightness" ] || exit 0
[ "$DIM_AFTER" -gt 0 ] || [ "$OFF_AFTER" -gt 0 ] || exit 0
[ -r "$IIO/in_accel_x_raw" ] || { logger -t utr-lcd "power: no accelerometer at $IIO, not dimming"; exit 0; }

MAX=$(cat "$BL/max_brightness" 2>/dev/null) || exit 0
mkdir -p "$RUN"

# set_level PERCENT: 0 turns the backlight off, anything else is at least 1 step
set_level() {
	local v=$(( MAX * $1 / 100 ))
	[ "$1" -gt 0 ] && [ "$v" -lt 1 ] && v=1
	echo "$v" > "$BL/brightness"
}

abs() { [ "$1" -lt 0 ] && echo $((0 - $1)) || echo "$1"; }

now() { local up; read -r up _ < /proc/uptime; echo "${up%.*}"; }

# each sysfs read blocks for about a second, so read the three axes in parallel
read_accel() {
	for a in x y z; do cat "$IIO/in_accel_${a}_raw" > "$RUN/accel.$a" 2>/dev/null & done
	wait
	x=$(cat "$RUN/accel.x") y=$(cat "$RUN/accel.y") z=$(cat "$RUN/accel.z")
	case "$x$y$z" in ''|*[!0-9-]*) return 1 ;; esac
}

set_level "$FULL"
state=full
last=$(now)
px="" py="" pz=""
while :; do
	if read_accel; then
		if [ -n "$px" ]; then
			dx=$(abs $((x - px))) dy=$(abs $((y - py))) dz=$(abs $((z - pz)))
			if [ "$dx" -gt "$THR" ] || [ "$dy" -gt "$THR" ] || [ "$dz" -gt "$THR" ]; then last=$(now); fi
		fi
		px=$x py=$y pz=$z
	fi
	idle=$(( $(now) - last ))
	want=full
	[ "$DIM_AFTER" -gt 0 ] && [ "$idle" -ge "$DIM_AFTER" ] && want=dim
	[ "$OFF_AFTER" -gt 0 ] && [ "$idle" -ge "$OFF_AFTER" ] && want=off
	if [ "$want" != "$state" ]; then
		case "$want" in full) set_level "$FULL" ;; dim) set_level "$DIM_LEVEL" ;; off) set_level 0 ;; esac
		state=$want
	fi
	sleep 1
done
