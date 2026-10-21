#!/bin/sh
# utr-lcd power manager: dims and/or blanks the backlight after a period without motion and restores
# it when the router is moved. Works with both display modes (it only touches the backlight).
# Motion comes from the on-board LIS2DW12 accelerometer (kernel IIO sysfs; the sampling rate is raised while running). BusyBox ash.
#
#   UTR_FULL       backlight percent while active (default 100)
#   UTR_DIM_AFTER  seconds without motion before dimming, 0 = never dim (default 0)
#   UTR_DIM_LEVEL  backlight percent while dimmed (default 15)
#   UTR_OFF_AFTER  seconds without motion before the backlight goes off, 0 = never (default 0)
#   UTR_WAKE_THR   change that counts as motion, in milli-g (default 50); the sensor reports ~1 raw unit per
#                  milli-g, noise is about +-6 at 25 Hz (+-15 at 1 Hz)
#   UTR_ODR        accelerometer sampling rate in Hz while running (default 25; 0 = leave it alone). At the
#                  driver default of 1 Hz every sysfs read blocks ~4 s, so motion is seen far too late
#   UTR_RUN        scratch dir (default /var/run/utr-lcd)
#   UTR_IIO        accelerometer directory (default /sys/bus/iio/devices/iio:device0)
BL=${UTR_BACKLIGHT:-/sys/class/backlight/backlight}
IIO=${UTR_IIO:-/sys/bus/iio/devices/iio:device0}
RUN=${UTR_RUN:-/var/run/utr-lcd}

num() { case "$1" in ''|*[!0-9]*) echo "$2" ;; *) echo "$1" ;; esac; }
FULL=$(num "$UTR_FULL" 100); DIM_AFTER=$(num "$UTR_DIM_AFTER" 0); DIM_LEVEL=$(num "$UTR_DIM_LEVEL" 15)
OFF_AFTER=$(num "$UTR_OFF_AFTER" 0); THR=$(num "$UTR_WAKE_THR" 50)
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

# Motion = the reading differs from a baseline by more than the threshold. The baseline is the last
# reading when motion was seen, refreshed every 60 s while idle to follow slow drift; comparing with a
# baseline (not with the previous sample) makes slow tilts and lifts add up instead of staying below
# the threshold in every single step.
ODR=$(num "$UTR_ODR" 25)
ODR_PREV=""
if [ "$ODR" -gt 0 ] && [ -w "$IIO/sampling_frequency" ]; then
	ODR_PREV=$(cat "$IIO/sampling_frequency" 2>/dev/null)
	echo "$ODR" > "$IIO/sampling_frequency" 2>/dev/null || ODR_PREV=""
fi
# leave the sensor and the backlight as a normal start would find them
cleanup() {
	[ -n "$ODR_PREV" ] && echo "$ODR_PREV" > "$IIO/sampling_frequency" 2>/dev/null
	set_level "$FULL"
	exit 0
}
trap cleanup TERM INT

set_level "$FULL"
state=full
last=$(now)
bx="" by="" bz="" base_t=$last
while :; do
	if read_accel; then
		if [ -z "$bx" ]; then
			bx=$x by=$y bz=$z base_t=$(now)
		else
			dx=$(abs $((x - bx))) dy=$(abs $((y - by))) dz=$(abs $((z - bz)))
			if [ "$dx" -gt "$THR" ] || [ "$dy" -gt "$THR" ] || [ "$dz" -gt "$THR" ]; then
				last=$(now)
				bx=$x by=$y bz=$z base_t=$last
			elif [ $(( $(now) - base_t )) -ge 60 ]; then
				bx=$x by=$y bz=$z base_t=$(now)
			fi
		fi
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
