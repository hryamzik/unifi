# UTR / UTR-LR stock LCD — findings from firmware 6.6.118

Source: `fw/BZ.ipq40xx_6.6.118+15541.260831.0545.bin`. Everything below was read from the
firmware image offline (nothing was run on the router). Re-extract with `ubnt_unpack.py` and
the steps at the end.

## Panel (stock device tree, `fdt@utr-lr` / config `ea08`)

```
st7789fb@2 {                 /* SPI0 CS2, 24 MHz */
    compatible = "st7789fb";
    dc-gpios = <&gpio 3>;  reset-gpios = <&gpio 0>;
    width = 135;  height = 240;  bpp = 16;
    colstart = 53;  rowstart = 40;
    rotation = 180;          /* UTR-LR only; the UTR dts is identical except for this line */
};
```

- Stock panel is **135x240**, GRAM offset **col 53 / row 40**. OpenWrt's `panel-mipi-dbi`
  uses 136x240 and no offset; a few edge pixels are clipped by the bezel.
- The 180 degree rotation we apply in software is exactly what stock does for the LR.
- Backlight: `pwm-backlight` on a software PWM (`pwm-gpio`, GPIO 2), levels 0..255, default 100.
- Other related hardware: LIS2DW12 accelerometer on i2c-gpio (raise-to-wake), TPS25751 USB-PD.

## Stock software stack

| Piece | Role |
| --- | --- |
| `/usr/bin/lcm-display` -> `mcad` | decides which screen to show; calls `ubus call ui ...` |
| `/usr/lib/liblvgl-lcm-middleware.so` | the UI itself ("Ulcd"), built on LVGL, draws to `/dev/fb0` |
| `st7789fb.ko` | fbdev driver (also has `burnin` and `breathing` sysfs hooks) |
| `/usr/etc/utr_api.sh` | `get-wan-status`, `get-client-status`, `set-lcm-config`, ... |
| `/var/run/lcm.conf`, `/tmp/lcm_state.conf` | settings and current-state hand-off |

## Screens (class names in the library)

Boot, Ready/Info (WPageUplink + WPageVpn pages), Blank, Night, Locate, Update, SetupReady
(QR page + MAC page), PinAuthentication, SetupProgress, plus factory tests
(backlight / burn-in / alignment / lines / colour).

## States chosen by `mcad` (priority in brackets where known)

`setupReady` ("Ready for Setup via App" + QR "Scan to Download UniFi"), `settingUp`,
`ready`, `noUplink` ("Searching..." / "No Signal"), `noInternet` ("No Internet"),
`captivePortal` ("Guest Portal via App"), `establishing.uplink` ("Establishing Connection"),
`update` ("Updating"), `factory.reset` ("Resetting to Factory Defaults"), `restart`
("Restarting"), `shutdown` ("Shutting Down"), `locate`, `pin.authentication` ("Enter this PIN on
your device"), night mode ("Entering Night Mode").

Connected sub-states: uplink is one of `eth0` (label **Ethernet**), `usb0` (**USB WAN**) or Wi-Fi
(`ath4` 5 GHz, `ath5` 2.4 GHz; SSID + signal in ` dBm`), each with optional VPN connecting /
connected. VPN type is Teleport, WireGuard or OpenVPN (label **Teleport** etc., "Connected",
"Connecting...", "Not Active").

## Data model passed to the UI (JSON from `mcad`)

`meshUplinkInterface`, `meshLinkSpeed`, `meshSsid`, `meshStatus`, `meshSignal`, `vpnStatus`,
`vpnName`, `vpnType`, `vpnConnectionTime`, `usbName`, `clients`, plus `bootTime`, `updateTime`,
`adopted`, `pin`, `mac`.

## Settings (`utr_api.sh set-lcm-config`)

- themes: blue (default) aqua green pink yellow orange purple
- brightness 1..100 (default 100); screen timeout 20..300 s (default 30) when raise-to-wake is on
- night mode begins/ends (HH:MM), raise-to-wake (accelerometer), motion sensitivity 1..5, timezone

## Web sources

- Display is a 1.14" non-touch status display: shows the repeated Wi-Fi network, uplink
  strength, client count and the linked UniFi site; a QR code during first setup
  (nascompares six-months review, dotjesper.com, techspecs.ui.com/unifi/wifi/utr).
- No brightness control before 6.6.115 added LCM management.

## Re-extract

```sh
python3 lcd/ubnt_unpack.py fw/BZ.ipq40xx_6.6.118+...bin out/        # SBL1, QSEE, u-boot
# kernel0 is a NAND-header + FIT at 0x11010c (size 0x1154a4b):
#   dumpimage -l kernel0.fit; kernel = lzop; kernel contains a zstd cpio initramfs at 0x7248a0
lzop -dc img0.bin > kernel.raw
tail -c +$((0x7248a0+1)) kernel.raw | zstd -dc | cpio -idm       # = stock rootfs
```
