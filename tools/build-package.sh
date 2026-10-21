#!/bin/sh
# Build the packages under package/ (utr-lcd, luci-app-utr-lcd) with the official OpenWrt SDK
# (docker) for ipq40xx/generic.
#
#   tools/build-package.sh [SDK_TAG]        default tag: ipq40xx-generic-master (snapshot -> .apk)
#   e.g. tools/build-package.sh ipq40xx-generic-24.10.4   -> .ipk for opkg-based releases
#
# The SDK is cached in the docker volume utr-sdk-<tag> (docker volume rm to reset).
# Output: build/sdk-out/<tag>/ (package files)
set -eu
cd "$(dirname "$0")/.."
TAG=${1:-ipq40xx-generic-master}
OUT=build/sdk-out/$TAG
PKGS="utr-lcd luci-app-utr-lcd"
mkdir -p "$OUT"
rm -f "$OUT"/*.apk "$OUT"/*.ipk
docker run --rm --platform linux/amd64 -e PKGS="$PKGS" \
	-v "utr-sdk-$TAG":/builder \
	-v "$PWD/package":/src:ro -v "$PWD/$OUT":/out \
	"openwrt/sdk:$TAG" bash -ec '
		[ -d package ] || bash ./setup.sh      # first run: downloads the SDK into the cached volume
		for p in $PKGS; do rm -rf "package/$p" && cp -r "/src/$p" "package/$p"; done
		make defconfig >/dev/null
		# some stable SDK images are built for another target with the same CPU architecture
		# (e.g. bcm2709, arm_cortex-a7_neon-vfpv4): the binaries are identical, only the
		# @TARGET_ipq40xx restriction must go
		if ! grep -q "^CONFIG_TARGET_ipq40xx=y" .config; then
			echo "NOTE: SDK target is not ipq40xx ($(grep -m1 ^CONFIG_TARGET_BOARD .config)); same arch, dropping @TARGET_ipq40xx"
			sed -i "s/@TARGET_ipq40xx //" package/utr-lcd/Makefile
		fi
		for p in $PKGS; do echo "CONFIG_PACKAGE_$p=m" >> .config; done
		make defconfig >/dev/null
		for p in $PKGS; do make "package/$p/compile" V=s -j"$(nproc)" 2>&1 | tail -5; done
		for p in $PKGS; do find bin -name "$p[-_]*" \( -name "*.apk" -o -name "*.ipk" \) -exec cp -v {} /out/ \; ; done
	'
ls -l "$OUT"
