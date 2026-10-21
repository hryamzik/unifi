#!/bin/sh
# Photograph the router's LCD with the iPhone (Continuity Camera) and save a JPEG.
#
#   tools/lcd-shot.sh OUT.jpg                      photo of whatever the display shows now
#   tools/lcd-shot.sh OUT.jpg PROG [ARGS...]       stop the utr-lcd service, run local armv7 PROG
#                                                  (e.g. build/extras/lcdgrid) on the router, photo,
#                                                  then stop PROG and start the service again
#
# Env: ROUTER (root@192.168.1.1), CAM_NAME (match, default "iPhone Camera"), CAM_ROT
# (rotation applied to the photo: 180, 0; default 180 = the viewing orientation of the LR),
# CAM_SIZE (1920x1440), CAM_SKIP (frames dropped while exposure settles, 45).
# The service is always restored on exit. Needs ffmpeg and camera permission for the calling app.
set -eu
OUT=${1:?usage: lcd-shot.sh OUT.jpg [PROG [ARGS...]]}; shift
ROUTER=${ROUTER:-root@192.168.1.1}
CAM_NAME=${CAM_NAME:-iPhone Camera}
CAM_ROT=${CAM_ROT:-180}
CAM_SIZE=${CAM_SIZE:-1920x1440}
CAM_SKIP=${CAM_SKIP:-45}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=5 $ROUTER"

cam=$(ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 \
	| sed -n 's/.*\[\([0-9]*\)\] \(.*\)$/\1 \2/p' | grep -F "$CAM_NAME" | grep -v "Desk View" | head -n1 | cut -d' ' -f1)
[ -n "$cam" ] || { echo "lcd-shot: no camera matching '$CAM_NAME'" >&2; exit 1; }

prog=""
restore() {
	if [ -n "$prog" ]; then
		$SSH "killall $(basename "$prog") 2>/dev/null; /etc/init.d/utr-lcd start" >/dev/null 2>&1 || \
			echo "lcd-shot: WARNING could not restart the service, run: ssh $ROUTER /etc/init.d/utr-lcd start" >&2
	fi
}
trap restore EXIT INT TERM

if [ $# -ge 1 ]; then
	prog=$1; shift
	name=$(basename "$prog")
	$SSH "cat > /tmp/$name && chmod +x /tmp/$name" < "$prog"
	# stop returns before the old process has released the DRM display: wait until it is gone
	$SSH "/etc/init.d/utr-lcd stop; killall $name 2>/dev/null
		i=0; while pidof utr-lcd-status utr-lcd-fb2drm >/dev/null 2>&1 && [ \$i -lt 20 ]; do sleep 1; i=\$((i+1)); done
		( /tmp/$name $* >/tmp/$name.log 2>&1 & ); sleep 2; cat /tmp/$name.log"
fi

case "$CAM_ROT" in 180) vf="hflip,vflip," ;; *) vf="" ;; esac
mkdir -p "$(dirname "$OUT")"
timeout 60 ffmpeg -hide_banner -loglevel error -y -f avfoundation -framerate 30 -video_size "$CAM_SIZE" -i "$cam:none" \
	-vf "${vf}select=gte(n\,$CAM_SKIP)" -frames:v 1 -update 1 -q:v 2 "$OUT"
echo "saved $OUT"
