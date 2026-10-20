#!/bin/sh
# Install or remove the optional stock-style LCD UI bundle.
#
#   utr-lcd-stock-install BUNDLE.tar.gz     install (bundle made on a PC by tools/make-stock-bundle.sh
#                                           from your own UniFi Travel Router firmware image)
#   utr-lcd-stock-install --remove          remove it (falls back to the built-in renderer)
#
# The bundle contains Ubiquiti's proprietary UI binaries and is NOT part of the utr-lcd package.
DEST=/usr/share/utr-lcd/stock

die() { echo "utr-lcd-stock-install: $*" >&2; exit 1; }

case "$1" in
--remove)
	rm -rf "$DEST"
	echo "stock bundle removed"
	/etc/init.d/utr-lcd restart 2>/dev/null
	exit 0 ;;
''|-h|--help) sed -n '2,10p' "$0"; exit 1 ;;
esac

[ -f "$1" ] || die "no such file: $1"
list=$(tar -tzf "$1" 2>/dev/null) || die "not a valid .tar.gz"
echo "$list" | grep -qE '(^/|\.\./)' && die "bundle contains unsafe paths"
echo "$list" | grep -qx 'utr-ui-stock/bin/ulcmd' || die "bundle has no utr-ui-stock/bin/ulcmd"
echo "$list" | grep -qx 'utr-ui-stock/lib/libc.so' || die "bundle has no utr-ui-stock/lib/libc.so"
echo "$list" | grep -qvE '^utr-ui-stock(/|$)' && die "unexpected content in bundle"

tmp=/usr/share/utr-lcd/.stock.new
rm -rf "$tmp"; mkdir -p "$tmp" || die "cannot create $tmp"
# BusyBox tar has no --strip-components: extract, then move the single top-level dir into place
tar -xzf "$1" -C "$tmp" || { rm -rf "$tmp"; die "extract failed"; }
rm -rf "$DEST"; mv "$tmp/utr-ui-stock" "$DEST" || { rm -rf "$tmp"; die "install failed"; }
rm -rf "$tmp"
chmod 755 "$DEST/bin/ulcmd" "$DEST/lib/libc.so"
echo "installed stock bundle ($(cat "$DEST/VERSION" 2>/dev/null)), $(du -sh "$DEST" | cut -f1)"
/etc/init.d/utr-lcd restart 2>/dev/null
