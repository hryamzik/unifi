#!/bin/sh
# Regenerate package/utr-lcd/src/fonts.h from Terminus (docker, alpine terminus-font package).
set -eu
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD/tools":/t:ro alpine:3.20 sh -c '
	apk add --no-cache python3 terminus-font >/dev/null 2>&1
	d=/usr/share/consolefonts
	python3 /t/psf2h.py t16=$d/ter-116n.psf.gz t16b=$d/ter-116b.psf.gz t24b=$d/ter-124b.psf.gz t32b=$d/ter-132b.psf.gz t12=$d/ter-112n.psf.gz' \
	> package/utr-lcd/src/fonts.h
wc -c package/utr-lcd/src/fonts.h
