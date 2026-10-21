#!/bin/sh
# Cross-build the hardware test programs as static armv7 binaries (docker, alpine) into build/extras/.
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/extras
docker run --rm --platform linux/arm/v7 -v "$PWD":/w alpine:3.20 sh -c '
	apk add --no-cache gcc musl-dev linux-headers libdrm-dev make >/dev/null 2>&1
	cd /w/tools/extras && make LDFLAGS=-static && mv lcdtest lcdorient lcdframes lcdgrid lcdfill /w/build/extras/ && make clean >/dev/null'
ls -l build/extras
