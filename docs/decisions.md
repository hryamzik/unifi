# Design decisions and hardware facts

Short records of why things are the way they are. Verified facts are marked (verified), guesses (hypothesis).

## Hardware (verified on the router unless noted)

- Panel: 136x240 ST7789-class LCD on SPI (`spi0.2`, DT node `ubnt,utr-lcd`), dc GPIO 3, reset GPIO 0, write-only. OpenWrt drives it with `panel-mipi-dbi-spi` (DRM); init sequence comes from `/lib/firmware/ubnt,utr-lcd.bin`.
- No `/dev/fb0`. Drawing uses DRM dumb buffers (XRGB8888): `SETCRTC` once, then `DIRTYFB` per update. Connector `SPI-1` needs the `SETCRTC`.
- Backlight: `/sys/class/backlight/backlight` (pwm, max 100).
- The panel is mounted upside down on the LR: stock sets `rotation=180`. We rotate in software (`lcd_open(..., flip_x, flip_y)`), config option `rotate`.
- Stock panel geometry is 135x240 with GRAM offset col 53 / row 40; OpenWrt uses 136x240 without offset. Photos showed the left ~12 px and right ~4 px of the logical view clipped, plus the bottom when viewed at an angle (hypothesis: the offset mismatch; not calibrated). The native layout works around it with margins (`MARGIN_L 12`, `MARGIN_R 4`); stock mode exposes `offset_x` / `offset_y` for tuning.
- Viewing angle matters: the panel looks different off-axis, so the native renderer uses black background, large high-contrast text, no 1 px lines, and words for every state.
- Router runs OpenWrt SNAPSHOT (apk); stable releases use opkg, hence both `.apk` and `.ipk` builds.

## Decisions

### Two display modes, native by default
The package works with the built-in renderer alone. The Ubiquiti-style UI is an add-on because the screens were a hard requirement from the user ("a mode mimicking the original one is a must") but the binaries are proprietary.

### Stock bundle is built from the user's own firmware, never shipped
`tools/make-stock-bundle.sh` finds the kernel (lzop in the FIT) and the zstd cpio initramfs by magic numbers (no fixed offsets; works on 6.6.115 and 6.6.118), resolves `ulcmd`'s DT_NEEDED closure, and writes `utr-ui-stock.tar.gz`. `utr-lcd-stock-install` installs it to `/usr/share/utr-lcd/stock`. Published packages and the git repo contain no Ubiquiti code.

### Stock UI runs under a shim, own loader, private ubus
- The stock binaries are soft-float ARM musl; OpenWrt is hard-float. They start through the bundle's own `libc.so --library-path`.
- `utr-lcd-fbshim.so` (LD_PRELOAD) fakes `/dev/fb0` as `/var/run/utr-lcd/fb0.raw` (135x240 RGB565), redirects `/proc/net/wireless` to a file we write (so the stock UI shows our real Wi-Fi signal as `ath4`), and refuses `system()`/`popen()` so the UI cannot run poweroff/reboot. It logs each refusal (`shim.log`); the only one seen is `mkdir -p /etc/ulcmd`.
- A private `ubusd` listens on `/var/run/ubus.sock`; OpenWrt's ubus (`/var/run/ubus/ubus.sock`) is not touched. The OpenWrt-native `ubusd`/`ubus` are used for it (verified compatible with the stock `ulcmd`), so the bundle only needs `ulcmd` and its libraries.
- `feed.sh` replaces Ubiquiti's `mcad`: it maps OpenWrt state to the stock `ui info` / `ui state` ubus calls (`ready`, `noUplink`, `noInternet`). Uplink type maps to `ath4` (Wi-Fi), `usb0`, `eth0`; WireGuard/Tailscale/OpenVPN interfaces map to the VPN page. It never calls shutdown/restart/reset states.
- `utr-lcd-fb2drm` copies `fb0.raw` to the panel through DRM (rotation, optional offset).

### procd supervises everything
`/etc/init.d/utr-lcd` starts either the native instance, or `ubusd` + `ulcmd` + `fb2drm` + `feed` as separate procd instances with respawn. Logs go to syslog (`logread`). The init script fails softly (log line) when `/dev/dri/card0` is missing.

### Package metadata
- No hard dependency on `kmod-drm-panel-mipi-dbi`: it ships with the device image, and declaring it makes the SDK build the whole kernel. Runtime check instead.
- `@TARGET_ipq40xx` is kept in the Makefile; `tools/build-package.sh` strips it only when an SDK image has a different target with the same CPU architecture (the 24.10.4 image is bcm2709, `arm_cortex-a7_neon-vfpv4`), so the resulting `.ipk` is binary-compatible with ipq40xx.
- Licence MIT; the font in `font8x16.h` is derived from Terminus (SIL OFL 1.1), noted in `package/utr-lcd/LICENSE`.

### Ownership and attribution
The repository owner, Roman Belyakovsky, is the sole author of all commits and PRs. See `AGENTS.md` for the working rules and `tools/commit.sh`.

## Open items

- Calibrate the panel window offset on the glass (hypothesis above).
- Screen timeout, night mode and raise-to-wake (stock settings, accelerometer) are not wired; brightness is applied at service start only.
- USB-tether, VPN and `noInternet` paths exist in `collect.sh`/`feed.sh` but are only tested offline.
- The UTR (non-LR) variant is untested; rotation differs there (`rotate 0`).
