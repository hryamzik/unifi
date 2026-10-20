#!/bin/sh
# Make the optional stock-UI bundle from your own UniFi Travel Router firmware image.
#
#   tools/make-stock-bundle.sh FIRMWARE.bin [OUT.tar.gz]
#
# Runs the extraction in an Alpine container (docker). Set NO_DOCKER=1 to use host tools
# (python3 lzop zstd cpio binutils). Install the result on the router with
# `utr-lcd-stock-install OUT.tar.gz`. The bundle contains Ubiquiti proprietary software.
set -eu
FW=${1:?usage: make-stock-bundle.sh FIRMWARE.bin [OUT.tar.gz]}
OUT=${2:-utr-ui-stock.tar.gz}
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "${NO_DOCKER:-0}" = 1 ]; then
	exec python3 "$HERE/extract_stock.py" "$FW" "$OUT"
fi

FWDIR=$(cd "$(dirname "$FW")" && pwd); FWBASE=$(basename "$FW")
OUTDIR=$(cd "$(dirname "$OUT")" && pwd); OUTBASE=$(basename "$OUT")
exec docker run --rm \
	-v "$HERE":/tools:ro -v "$FWDIR":/fw:ro -v "$OUTDIR":/out \
	alpine:3.20 sh -c "apk add --no-cache python3 lzop zstd cpio binutils >/dev/null && \
		python3 /tools/extract_stock.py /fw/$FWBASE /out/$OUTBASE"
