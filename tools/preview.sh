#!/bin/sh
# Render the built-in status screen offline (no router): compiles lcd-status.c with the mock display
# backend in docker, runs it on a collector (default: a fake one), writes a PNG as the viewer would
# see it, with the measured bezel window greyed out.
#   tools/preview.sh OUT.png [COLLECTOR_SCRIPT]       (collector path relative to the repo root)
set -eu
cd "$(dirname "$0")/.."
OUT=${1:?usage: preview.sh OUT.png [collector.sh]}
COLL=${2:-build/fake-collect.sh}
mkdir -p "$(dirname "$OUT")"
docker run --rm -v "$PWD":/w alpine:3.20 sh -c "
	apk add --no-cache gcc musl-dev python3 >/dev/null 2>&1
	gcc -O2 -Wall -Wextra -std=gnu11 -DLCD_MOCK -DLCD_ONCE_HOLD=0 -o /tmp/mock /w/package/utr-lcd/src/lcd-status.c
	LCD_MOCK_OUT=/tmp/p.ppm /tmp/mock -c /w/$COLL -1
	python3 /w/tools/preview.py /tmp/p.ppm /w/$OUT"
