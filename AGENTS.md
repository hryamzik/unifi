# Repository Guidelines

LCD status display (OpenWrt packages `utr-lcd` and `luci-app-utr-lcd`) for the UniFi Travel Router Long-Range (UTR-LR, IPQ4018). C programs, BusyBox shell scripts, an OpenWrt package Makefile.

## Commands

Build packages (apk, snapshot): `tools/build-package.sh` -> `build/sdk-out/ipq40xx-generic-master/*.apk` (both packages) (docker, first run downloads the SDK into a cached volume)
Build package (ipk, opkg releases): `tools/build-package.sh ipq40xx-generic-24.10.4` -> `build/sdk-out/ipq40xx-generic-24.10.4/*.ipk`
Build C only (Linux, DRM headers needed): `make -C package/utr-lcd/src` (verified in alpine with `libdrm-dev`); hardware test programs: `make -C tools/extras`
Look at the panel yourself: `tools/lcd-shot.sh build/cam/<new-name>.jpg [PROG [ARGS]]` (iPhone camera on USB pointed at the panel; see `docs/development.md`); offline preview of every native screen (calibrated bezel, no photo needed): `tools/preview.sh OUT.png`; regenerate fonts: `tools/gen-fonts.sh`
Build stock bundle from a firmware image: `tools/make-stock-bundle.sh fw/<firmware>.bin build/utr-ui-stock.tar.gz` (docker)
Syntax-check scripts: `sh -n package/utr-lcd/files/*.sh package/utr-lcd/files/utr-lcd.init package/luci-app-utr-lcd/root/usr/libexec/utr-lcd/luci-status`
Deploy to the router: `ssh root@192.168.1.1 'cat > /tmp/utr-lcd.apk' < build/sdk-out/ipq40xx-generic-master/utr-lcd-*.apk`, then `ssh root@192.168.1.1 'apk add --allow-untrusted --force-reinstall /tmp/utr-lcd.apk'`
LuCI pages: Services > LCD Display (`package/luci-app-utr-lcd`, view `htdocs/luci-static/resources/view/utr-lcd/settings.js`) and Services > LCD Screens (`screens.js`, editor with preview from `/usr/libexec/utr-lcd/luci-preview`; its JS height table must match `widget_h` in `lcd-status.c`); it cannot be exercised headlessly without the router's LuCI password, so ask the user to open it after installing.
Router logs: `ssh root@192.168.1.1 "logread | grep -E 'utr-lcd|stock-run|feed.sh'"`
Service: `/etc/init.d/utr-lcd start|stop|restart` on the router; config in `/etc/config/utr-lcd`
No unit tests exist. A change is verified by building the package, installing it, and checking processes and logs on the router; looking at the panel itself needs the user.

## Router and safety

Router: `ssh root@192.168.1.1` with the existing key; never ask for or print credentials. Verify reachability first, the address can change.
Autonomous: read-only SSH, logs, source research, firmware download and offline extraction, writing code and docs on this computer, building packages, RAM-only runs under `/tmp` and `/var/run`.
Ask first: package install or removal on the router, any persistent router configuration, flash/MTD writes, reboot, bootloader changes, kernel modules, GPIO/SPI/I2C pokes. The user's approval for one of these covers that action only.
Run Ubiquiti binaries only through `package/utr-lcd/files/stock-run.sh` (own musl loader, `utr-lcd-fbshim.so` refusing `system()`/`popen()`, private ubus). Call the stock UI through `ui-call.sh`, which exposes state/info/userActivity/locate and nothing else.
Keep SSH and LAN reachable and the stock recovery partition (`kernel0`) untouched; never write `kernel0`, bootloader, ART, EEPROM or boot-selection partitions.
State verified facts as verified and label hypotheses as hypotheses (open ones are listed in `docs/decisions.md`).

## Git and ownership

Roman Belyakovsky (`git config user.name` / `user.email`) is the only author and committer of every commit and the only author of every pull request.
Commit messages and PR bodies carry no `Co-authored-by`, `Generated with`, tool or AI attribution lines, and no mention of the assistant.
The Cursor agent environment appends `Co-authored-by: Cursor` to messages made with `git commit`; make commits with `tools/commit.sh "subject" ["body"]` (plumbing, no trailer injection) after `git add`.
After committing run `git log --format=%B | grep -ci co-authored`; the count must be 0. If a trailer slipped in, rewrite only your own unpushed commits with `git commit-tree`, tell the user, and leave the reflog intact.
Open PRs from the user's account (`gh pr create`, authenticated as the user) with title and body written in the user's voice; draft first.
History rewrites beyond your own unpushed commits, force-pushes, and pushes to shared branches need the user's explicit go-ahead.
Commit subjects are short imperative lines; one logical change per commit.

## Code conventions

Router shell scripts run on BusyBox ash: no bashisms, no `tar --strip-components`, no `eval` of collected data (parse `key=value` lines).
C code is raw DRM ioctls, no libdrm; it must build with the OpenWrt toolchain and warning-free on `-Wall -Wextra`.
Never commit Ubiquiti material: the stock bundle, firmware images, or anything extracted from them. `.gitignore` covers `fw/`, `build/`, `scratch/` and `utr-ui-stock*.tar.gz`; keep it that way.
The native renderer is the default and must keep working with nothing but this package installed; stock mode is optional.

## Docs

`docs/decisions.md`: why the design is the way it is (hardware facts, stock bundle split, shim, private ubus); read before changing architecture or stock-mode behaviour.
`docs/development.md`: day-to-day workflow, deploy and debug loop, gotchas found so far; read before touching the router.
`docs/stock-ui-notes.md`: what the stock firmware's LCD stack contains (states, data model, settings); read when extending stock mode.
`README.md`: user-facing install and configuration.

## Maintenance

Keep AGENTS.md and `docs/` updated when commands, tooling, router facts or the ownership rules change.
