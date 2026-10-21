# Development workflow

## Layout

```
package/utr-lcd/        OpenWrt package (Makefile, src/ C sources, files/ runtime scripts + init + config)
package/luci-app-utr-lcd/  LuCI settings/status page (view JS, menu.d, acl.d, status helper)
tools/                  host tools: build-package.sh, make-stock-bundle.sh, extract_stock.py, commit.sh,
                        psf2h.py (font), rgb565_to_png.py (framebuffer dump -> png), extras/ (hardware tests)
docs/                   decisions.md, development.md, stock-ui-notes.md
fw/ build/ scratch/     local only, git-ignored (firmware images, build output, scratch work)
```

## Edit, build, deploy, check

1. Edit under `package/utr-lcd/`.
2. `sh -n package/utr-lcd/files/*.sh package/utr-lcd/files/utr-lcd.init`
3. `tools/build-package.sh`; the C code compiles with the OpenWrt toolchain, so SDK warnings are real.
4. Deploy with the two `ssh` commands from `AGENTS.md` (asks the user first: it is a router install).
5. `ssh root@192.168.1.1 "ps | grep -E 'ubusd -s|libc.so|utr-lcd|feed.sh'; logread | grep -E 'utr-lcd|stock-run|feed.sh' | tail"`
6. Ask the user to look at the panel; photos from the user are the only visual feedback, taken at ~90 degrees.

Quick RAM-only experiments: copy a binary to `/tmp`, stop the service first (`/etc/init.d/utr-lcd stop`; only one process can drive the DRM panel), run it, restart the service afterwards. BusyBox has no `nohup`; use `(cmd &)`.

Offline screen captures of the stock UI (no router): run `ulcmd` from a bundle under qemu-user in docker with `utr-lcd-fbshim.so`, feed it with `ubus call ui ...`, convert `fb0.raw` with `tools/rgb565_to_png.py in.raw out.png 135 240 3`.

## Gotchas found so far

- BusyBox `tar` lacks `--strip-components`; BusyBox has no `nohup`; fractional `sleep` is not guaranteed.
- macOS `sed -i` needs `-i ''`; a failed `sed` in an `&&` chain silently skips the rest.
- `ssh ... 'cat > file'` takes one stdin per invocation; copy files in separate ssh calls.
- The panel needs `SETCRTC` once before `DIRTYFB` has any effect; there is no fbdev.
- Docker on this Mac is OrbStack (`open -a OrbStack`); the first pull may show a keychain prompt that the user must approve.
- The OpenWrt SDK images download the SDK at first run; `build-package.sh` caches it in the docker volume `utr-sdk-<tag>`. Depending on `kmod-*` in the package makes the SDK compile the kernel; do not.
- The stock rootfs has absolute symlinks (`/var -> /tmp`); never extract it with host paths resolved through them (use `cpio --no-absolute-filenames` in a throwaway dir, as `extract_stock.py` does).
- Stock `ulcmd` states `factory.reset`, `restart`, `shutdown` latch inside the daemon; do not send them.
- The `git commit` command in the Cursor agent environment adds a `Co-authored-by` trailer; use `tools/commit.sh`.

## Committing

`git add <paths>` then `tools/commit.sh "Subject" ["Body paragraph"]`. It commits the index on the current branch as the configured git user with no added trailers and refuses to proceed if the message contains `Co-authored-by`. Verify with `git log --format=%B | grep -ci co-authored` (expect 0).

## Pull requests

PRs are authored by the repository owner. Use `gh pr create --draft` from the owner's authenticated `gh`; write title and body as the owner would, with no assistant or tool attribution and no `Co-authored-by` lines. Do not push, open, or merge anything without the owner asking.
