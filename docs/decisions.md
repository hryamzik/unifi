# Design decisions and hardware facts

Short records of why things are the way they are. Verified facts are marked (verified), guesses (hypothesis).

## Hardware (verified on the router unless noted)

- Panel: 136x240 ST7789-class LCD on SPI (`spi0.2`, DT node `ubnt,utr-lcd`), dc GPIO 3, reset GPIO 0, write-only. OpenWrt drives it with `panel-mipi-dbi-spi` (DRM); init sequence comes from `/lib/firmware/ubnt,utr-lcd.bin`.
- No `/dev/fb0`. Drawing uses DRM dumb buffers (XRGB8888): `SETCRTC` once, then `DIRTYFB` per update. Connector `SPI-1` needs the `SETCRTC`.
- Backlight: `/sys/class/backlight/backlight` (pwm, max 100).
- The panel is mounted upside down on the LR: stock sets `rotation=180`. We rotate in software (`lcd_open(..., flip_x, flip_y)`), config option `rotate`.
- Stock panel geometry is 135x240 with GRAM offset col 53 / row 40; OpenWrt uses 136x240 without offset. Calibrated with a straight-on photo of a coordinate grid (`tools/extras/lcdgrid.c`): the visible window is raw x 1..130 and y 3..237. In viewer coordinates (after the 180 degree rotation) about 5 px are hidden at the left, 1 at the right, 2 at the top and 3 at the bottom, and the corners are circles of radius about 31 px (fitted from a photo of a white grid, `tools/extras/lcdfill.c` + `tools/measure-bezel.py`, rms 0.5 px on the two well-lit corners; an earlier guess of 16 px made the offline previews too generous, text near the bottom corners was cut on the glass but not in the preview). The offline preview (`tools/preview.py`) uses that mask, and a photo of the live display matches it, so previews are trustworthy for layout work; photos are only needed for colour, brightness and viewing-angle questions. The visible window shifts by a few pixels with the camera angle (the glass is recessed), so treat edges as +-2 px. The native renderer takes its margins from that (`clip_l`/`clip_r` in `lcd-status.c`); the stock 135 px wide image is centred in the window with `offset_x` 2 (verified with the camera; `offset_y` 0).
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
- The UI applies a setting (theme, brightness, ...) only after saving it to `/etc/ulcmd/ulcmd.conf` (`key=value` lines). That directory does not exist on OpenWrt, so every change used to be dropped silently (verified: themes did not change although `cb_Theme` was delivered). The shim redirects that path to `$FBSHIM_CONF` (`/var/run/utr-lcd/ulcmd.conf`, RAM), which keeps flash untouched; settings are pushed again by `feed.sh` on every start.
- A private `ubusd` listens on `/var/run/ubus.sock`; OpenWrt's ubus (`/var/run/ubus/ubus.sock`) is not touched. The OpenWrt-native `ubusd`/`ubus` are used for it (verified compatible with the stock `ulcmd`), so the bundle only needs `ulcmd` and its libraries.
- `feed.sh` replaces Ubiquiti's `mcad`: it maps OpenWrt state to the stock `ui info` / `ui state` ubus calls (`ready`, `noUplink`, `noInternet`). Uplink type maps to `ath4` (Wi-Fi), `usb0`, `eth0`; WireGuard/Tailscale/OpenVPN interfaces map to the VPN page. It never calls shutdown/restart/reset states.
- `utr-lcd-fb2drm` copies `fb0.raw` to the panel through DRM (rotation, optional offset).

### Native renderer: screens, widgets, playlist
Designed in a long review with the owner (the questions and answers are in the chat history; the outcome is here).
- A **screen** is a stack of **widgets** with fixed pixel heights (text sizes 16/24/32, rows 12/16), drawn over an optional **Wi-Fi signal backfill** (vertical fill from the bottom, red to green, dimmed to `fill_dim` percent, optional bright edge line; Wi-Fi uplink only, it costs no space) and a **status indicator** (dot, grown into a text banner on NO UPLINK / NO INTERNET; or a full pill; or none). Every screen has its own `dwell` (0.5 s minimum), an `enabled` flag, and the rotation follows a `playlist` (`1 2 1 3`, empty = all enabled screens) or pins one screen.
- Layout is computed on every draw from what there is to show: a widget with no data (no VPN configured, no signal) takes no room, spare room is spread evenly (max 14 px gaps), and each row's usable width is reduced by the measured corner circle (r=31) at its own y, so nothing sits in a cut corner.
- Fit policy: text steps down the font ladder (32 -> 24 -> 16 -> 12 px) before anything is cut; only then `..` replaces the end (names) or the middle (IP addresses). All rows of a screen share one step so they stay aligned. Numbers must never be drawn over charts.
- Config: `/etc/config/utr-lcd` (`config screen` sections, `list widget 'type:size'`) is flattened by the init script into `/var/run/utr-lcd/screens.conf` (key=value, parsed by the C renderer, same file feeds the offline preview). No file or no screens: built-in defaults (same two screens), so the renderer works standalone.
- The collector runs in the background and is read without blocking, so 0.5 s dwells do not stutter. The screen is redrawn only when the screen or the data changed.
- Traffic (M2, done): `traffic.sh` (one awk process fed by a shell loop, every 2 s) reads the uplink device counters, the wired bridge ports and `iw dev X station dump` per Wi-Fi station, sums deltas over counter resets (a re-associating station restarts at 0) and rewrites `/var/run/utr-lcd/traffic` atomically; RAM only, so no flash wear. The init script starts it only if a screen that will be shown (enabled, in the playlist, or pinned) has a traffic widget. Wired ports are merged into one client `LAN` (per-port MACs would need conntrack or an ARP join, not worth the CPU); per-client numbers count both directions. The renderer reads the file once a second while a traffic screen is on show and ignores it when older than 15 s. Client colour = FNV hash of the id into a 12-colour palette with probing, so it is stable while the client set is. Horizontal share bars are the default, the donut is optional, numbers are never drawn on a chart (the donut puts the total in the hole, in the largest font that fits). Bars and donut show the six biggest clients and a grey rest; the legend keeps its last row for `+k more`, rows near the rounded corners get less width and drop the name before they could overlap the value.
- Resources (M4, done): `cpu` (/proc/stat delta, needs two samples), `ram` (MemAvailable), `flash` (statvfs /overlay), `temp` (first hwmon temp1_input, the ath10k chip; hidden without one) are read inside the renderer once a second while the screen on show uses them: no extra processes. Rows of one screen share one text size, resource rows included.
- Multi-WAN (M4, partly verified): `collect.sh` lists every default route from `ip route show table all` (lowest metric first, VPN devices skipped, max 3) as `upN_*`; `traffic.sh` counts each uplink device and writes `uplink=` lines. `multiwan` shows each uplink's share of the current traffic; a higher-metric uplink with no traffic reads `backup`. This only runs on a single uplink on the test router (the wired WAN is down); the multi-uplink paths are verified with the fake collector and fake traffic in the preview, not on real mwan3 or failover hardware (hypothesis: mwan3 policy tables carry the per-interface default routes, so `table all` finds them). The Wi-Fi backfill is split into equal vertical strips per Wi-Fi uplink.
- Planned (see the milestones in the chat): M3 LuCI editor with live preview, M4 multi-WAN (backfill split horizontally per Wi-Fi uplink), resources widgets, VPN states, M5 upstream packaging.

### procd supervises everything
`/etc/init.d/utr-lcd` starts either the native instance, or `ubusd` + `ulcmd` + `fb2drm` + `feed` as separate procd instances with respawn. Logs go to syslog (`logread`). The init script fails softly (log line) when `/dev/dri/card0` is missing.

### LuCI app is a separate, pure-JS package
`luci-app-utr-lcd` (arch `all`) holds only a form view over `/etc/config/utr-lcd`, a menu entry, an rpcd ACL and `/usr/libexec/utr-lcd/luci-status` (read-only JSON status, run through rpcd's file `exec` ACL like LuCI's own package manager helper). No rpcd plugin and no Lua/ucode, so it works on snapshot and 24.10. Applying settings relies on the init script's `procd_add_reload_trigger utr-lcd` (verified: `uci commit` + `reload_config` restarts the service with the new values). Uploading the stock bundle from the browser is deliberately not offered: it would run a root installer on an uploaded file.

### Package metadata
- No hard dependency on `kmod-drm-panel-mipi-dbi`: it ships with the device image, and declaring it makes the SDK build the whole kernel. Runtime check instead.
- `@TARGET_ipq40xx` is kept in the Makefile; `tools/build-package.sh` strips it only when an SDK image has a different target with the same CPU architecture (the 24.10.4 image is bcm2709, `arm_cortex-a7_neon-vfpv4`), so the resulting `.ipk` is binary-compatible with ipq40xx.
- Licence MIT; the font in `font8x16.h` is derived from Terminus (SIL OFL 1.1), noted in `package/utr-lcd/LICENSE`.

### Ownership and attribution
The repository owner, Roman Belyakovsky, is the sole author of all commits and PRs. See `AGENTS.md` for the working rules and `tools/commit.sh`.

## Open items

- Night mode (`nightModeBegins`/`nightModeEnds` in `ui info`) is not wired or verified.
- Idle dimming, blanking and wake-on-motion are our own `power.sh` (both modes, backlight only): it reads the LIS2DW12 accelerometer. At the driver's default 1 Hz each sysfs read blocks about 4 s and parallel reads queue (one sample per ~12 s, verified), so it raises `sampling_frequency` to 25 Hz while running (5 samples in 2 s, verified) and restores the old rate on stop; the baseline is refreshed every 60 s and treats a change of more than `motion_threshold` (default 50 milli-g; the sensor gives about 1 raw unit per milli-g; noise +-15 at the driver's 1 Hz, +-6 at 25 Hz) as motion; the default is a guess, tune it by handling the router. The stock UI's own timeout is kept off (`raiseToWake` false) because it is all-or-nothing (dim then off, no way to stay dimmed) and left the screen dark after a restart with timeout 0. The init script also restores a zero backlight on every start.
- USB-tether, VPN and `noInternet` paths exist in `collect.sh`/`feed.sh` but are only tested offline.
- The UTR (non-LR) variant is untested; rotation differs there (`rotate 0`).
