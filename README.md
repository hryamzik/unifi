# utr-lcd - LCD status display for the UniFi Travel Router (OpenWrt)

Drives the built-in 1.14" 136x240 LCD of the **UniFi Travel Router Long-Range** (UTR-LR, IPQ4018)
running OpenWrt. It shows the Wi-Fi/Ethernet/USB uplink, signal, WAN IP, VPN state, connected
clients and uptime, and survives reboots as a normal procd service.

Two display modes (`mode` in `/etc/config/utr-lcd`):

| Mode | What you get | Needs |
| --- | --- | --- |
| `native` | Built-in high-contrast status screen, tuned for off-axis viewing | nothing, ships in the package |
| `stock` | The original Ubiquiti UI (pixel-identical, themes, signal bars, ...) fed with OpenWrt data | optional bundle you build from **your own** firmware |

`auto` (default) uses `stock` when the bundle is installed, otherwise `native`.

## Requirements

- OpenWrt on the UTR / UTR-LR with the LCD panel driver (`kmod-drm-panel-mipi-dbi`, device tree node
  `ubnt,utr-lcd`) - `/dev/dri/card0` must exist.
- `iwinfo` command (default in OpenWrt images) for Wi-Fi uplink details.

## Install

Build for your release with the OpenWrt SDK (see below), copy the package to the router and:

```sh
# snapshot / apk-based releases
apk add --allow-untrusted ./utr-lcd-*.apk
# opkg-based releases
opkg install ./utr-lcd_*.ipk
```

The service starts immediately and at boot. `service utr-lcd restart` after editing the config.

### Optional: stock Ubiquiti look

Ubiquiti's UI binaries are proprietary and cannot be redistributed, so build the bundle from a
firmware image you downloaded yourself (any 6.6.x UTR firmware from ui.com):

```sh
tools/make-stock-bundle.sh UTR-6.6.118.bin utr-ui-stock.tar.gz      # needs docker
scp utr-ui-stock.tar.gz root@192.168.1.1:/tmp/
ssh root@192.168.1.1 utr-lcd-stock-install /tmp/utr-ui-stock.tar.gz
utr-lcd-stock-install --remove                                      # go back to native
```

The stock `ulcmd` runs from its own musl loader and libraries (it is a soft-float ARM binary),
under a small `LD_PRELOAD` shim (`utr-lcd-fbshim.so`) that fakes `/dev/fb0`, feeds it a stock-style
Wi-Fi signal, and **refuses `system()`/`popen()`** so the UI can never reboot or power off the router.
It talks to a private ubus (`/var/run/ubus.sock`); OpenWrt's own ubus is untouched. Nothing is run
from flash except the bundle itself (~7 MB in `/usr/share/utr-lcd/stock`).

## Configuration (`/etc/config/utr-lcd`)

| Option | Default | Meaning |
| --- | --- | --- |
| `enabled` | `1` | start the service |
| `mode` | `auto` | `auto`, `native`, `stock` |
| `interval` | `3` | seconds between status refreshes |
| `rotate` | `180` | panel rotation (`180` on the UTR-LR, which is mounted upside-down; or `0`) |
| `brightness` | empty | backlight percent 1-100, applied when the service starts; empty leaves it alone |
| `theme` | `blue` | stock mode: `blue aqua green pink yellow orange purple` |
| `offset_x`, `offset_y` | `0` | stock mode: placement of the 135x240 image in the 136x240 panel |

## How it works

- The panel is driven through DRM (`panel-mipi-dbi`, write-only): the renderer makes a dumb buffer,
  sets the CRTC once and updates with `DIRTYFB`. Raw ioctls, no libdrm; orientation is done in software.
- `collect.sh` gathers the state (default-route uplink, Wi-Fi via `iwinfo`, VPN by interface name,
  clients from ARP, uptime). `utr-lcd-status` renders it (native) or `feed.sh` pushes it to the stock
  UI's `ui state` / `ui info` ubus methods (stock; replaces Ubiquiti's `mcad`).
- `utr-lcd-fb2drm` copies the stock UI's fake framebuffer to the panel.

Findings about the stock firmware are in `docs/stock-ui-notes.md`.

## Building

```sh
tools/build-package.sh                                # snapshot SDK  -> build/sdk-out/*/utr-lcd-*.apk
tools/build-package.sh ipq40xx-generic-24.10.4        # opkg release -> .ipk
make -C package/utr-lcd/src CC=...                    # just the C programs
```

## Licence

MIT (see `package/utr-lcd/LICENSE`). The 8x16 font is derived from Terminus (SIL OFL 1.1).
UniFi and Ubiquiti are trademarks of Ubiquiti Inc.; this project is not affiliated with Ubiquiti.
