#!/bin/sh
# Build the utr-lcd package with the official OpenWrt SDK (docker) for ipq40xx/generic.
#
#   tools/build-package.sh [SDK_TAG]        default tag: ipq40xx-generic-master (snapshot -> .apk)
#   e.g. tools/build-package.sh ipq40xx-generic-24.10.0   -> .ipk for opkg-based releases
#
# The SDK is cached in the docker volume utr-sdk-<tag> (docker volume rm to reset).
# Output: build/sdk-out/<tag>/ (package files + index)
set -eu
cd "$(dirname "$0")/.."
TAG=${1:-ipq40xx-generic-master}
OUT=build/sdk-out/$TAG
mkdir -p "$OUT"
docker run --rm --platform linux/amd64 \
	-v "utr-sdk-$TAG":/builder \
	-v "$PWD/package/utr-lcd":/src:ro -v "$PWD/$OUT":/out \
	"openwrt/sdk:$TAG" bash -ec '
		[ -d package ] || bash ./setup.sh      # first run: downloads the SDK into the cached volume
		rm -rf package/utr-lcd && cp -r /src package/utr-lcd
		make defconfig >/dev/null
		echo "CONFIG_PACKAGE_utr-lcd=m" >> .config
		make defconfig >/dev/null
		make package/utr-lcd/compile V=s -j"$(nproc)" 2>&1 | tail -40
		find bin -name "utr-lcd*" -exec cp -v {} /out/ \;
	'
