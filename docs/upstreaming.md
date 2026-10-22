# Upstreaming plan

Goal: the LCD packages installable from official OpenWrt feeds, and eventually part of the
stock image for the UniFi Travel Router (`ubnt_utr`, ipq40xx/generic — the device itself is
already supported upstream in snapshots).

## Stage 1 — this repository (done)

- Public repo: https://github.com/hryamzik/unifi (`main` pushed, packages build from the SDK
  with `tools/build-package.sh`; `tools/build-image.sh` assembles an image with the packages
  baked in). Work happens on branches with draft PRs from here on.

## Stage 2 — feed-ready packaging

`utr-lcd` → `openwrt/packages`, `luci-app-utr-lcd` → `openwrt/luci`. What the current
package Makefiles still lack for that (see `docs/decisions.md`, "M5"):

- [ ] Source as a release tarball from this repo with `PKG_SOURCE`, `PKG_SOURCE_VERSION`
      and `PKG_HASH` (today the Makefile compiles straight from the feed directory, which
      only works inside this repo's SDK overlay).
- [ ] Decide the source layout for a monorepo: the feed clones this repo and builds the two
      package directories, or the packages move to their own repos. (Feed precedent exists
      for git sources with `PKG_SOURCE_SUBDIR`; check what reviewers accept now.)
- [ ] `PKG_LICENSE`/`PKG_LICENSE_FILES`, `PKG_MAINTAINER` (present), `PKG_RELEASE` bumps
      per change, `Signed-off-by` on every upstream commit.
- [ ] LuCI app via `luci.mk` (`LUCI_TITLE`, `LUCI_DEPENDS`, `LUCI_PKGARCH`) instead of the
      hand-rolled install rules, plus i18n scaffolding (`po/` template) even if English-only.
- [ ] Keep the deliberate non-dependencies: no `kmod-drm-*` hard dependency (runtime
      `/dev/dri/card0` check instead, see decisions.md) — expect reviewer questions, the
      rationale is written down there.
- [ ] Verify builds for both package formats: snapshot/apk (master) and 24.10/ipk
      (`tools/build-package.sh ipq40xx-generic-24.10.4`), and a runtime test report for the
      upstream PR (router photos + `luci-preview` renders).

## Stage 3 — in the default image

- [ ] After stage 2 lands: PR to `openwrt/openwrt` adding `utr-lcd luci-app-utr-lcd` (plus
      `luci` if the profile lacks it) to `DEVICE_PACKAGES` of `ubnt_utr` in
      `target/linux/ipq40xx/image/generic.mk`, so a stock snapshot image drives the panel
      out of the box.
- [ ] Until then, `tools/build-image.sh` produces that image locally, and the README
      documents the manual path (flash upstream OpenWrt, install the two packages).
