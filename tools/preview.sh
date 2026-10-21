#!/bin/sh
# Render the built-in screens offline (no router): compiles lcd-status.c with the mock display backend in
# docker, runs it on a collector (default: a fake one) and the screen layout (default: the built-in
# screens), writes one PNG with every screen side by side, as the viewer would see it, with the measured
# bezel window greyed out.
#   tools/preview.sh OUT.png [COLLECTOR_SCRIPT [SCREENS.conf]]      (paths relative to the repo root)
# Every configured screen is drawn (also disabled ones); ONLY=2 draws just screen 2; TRAFFIC=file replaces tools/fake-traffic. FAKE_SIGNAL, FAKE_VPN, FAKE_WAN_IP, FAKE_SSID steer the fake collector.
set -eu
cd "$(dirname "$0")/.."
OUT=${1:?usage: preview.sh OUT.png [collector.sh [screens.conf]]}
COLL=${2:-tools/fake-collect.sh}
CONF=${3:-}
mkdir -p "$(dirname "$OUT")"
fake=$(env | sed -n 's/^\(FAKE_[A-Z_]*\)=.*/-e \1/p')   # FAKE_* variables reach the fake collector
docker run --rm -v "$PWD":/w -e ONLY="${ONLY:-}" -e CONF="$CONF" -e TRAFFIC="${TRAFFIC:-}" $fake alpine:3.20 sh -c '
	apk add --no-cache gcc musl-dev python3 >/dev/null 2>&1
	gcc -O2 -Wall -Wextra -std=gnu11 -DLCD_MOCK -DLCD_ONCE_HOLD=0 -o /tmp/mock /w/package/utr-lcd/src/lcd-status.c
	c=""; [ -n "$CONF" ] && c="-C /w/$CONF"
	n=$(/tmp/mock $c -l | wc -l); files=""
	for i in $(seq 1 $n); do
		[ -n "$ONLY" ] && [ "$ONLY" != "$i" ] && continue
		LCD_TRAFFIC_ANY_AGE=1 LCD_MOCK_OUT=/tmp/p$i.ppm /tmp/mock -T /w/${TRAFFIC:-tools/fake-traffic} -c /w/'"$COLL"' $c -1 -N $i
		files="$files /tmp/p$i.ppm"
	done
	python3 /w/tools/preview.py $files /w/'"$OUT"
