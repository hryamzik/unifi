#!/usr/bin/env python3
"""Build the optional stock-UI bundle for utr-lcd from a Ubiquiti UniFi Travel Router firmware.

  extract_stock.py FIRMWARE.bin OUT.tar.gz

The firmware is located structurally (no fixed offsets): it contains a FIT image whose kernel is
lzop-compressed, and that kernel embeds a zstd-compressed cpio initramfs which is the stock root
filesystem. The bundle gets Ubiquiti's `ulcmd` LCD daemon plus the shared libraries it needs
(resolved from DT_NEEDED), in a flat lib dir. Needs: lzop zstd cpio readelf (binutils).

The result contains Ubiquiti proprietary software: use it only on your own device, do not publish.
"""
import hashlib, os, re, shutil, subprocess, sys, tarfile, tempfile

LZOP_MAGIC = b"\x89LZO\x00\r\n\x1a\n"
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"


def run(cmd, data=None, **kw):
    return subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, **kw)


def find_kernel(fw):
    for m in re.finditer(re.escape(LZOP_MAGIC), fw):
        out = run(["lzop", "-dc"], fw[m.start():]).stdout   # trailing data makes lzop exit 1; output is fine
        if len(out) > 4_000_000 and b"Linux version" in out:
            print(f"kernel: lzop stream at 0x{m.start():x}, {len(out)} bytes")
            return out
    sys.exit("no lzop-compressed Linux kernel found in firmware")


def find_rootfs(kernel):
    for m in re.finditer(re.escape(ZSTD_MAGIC), kernel):
        out = run(["zstd", "-dc"], kernel[m.start():]).stdout
        if out[:6] == b"070701" and b"usr/sbin/ulcmd" in out:
            print(f"rootfs: zstd cpio at kernel offset 0x{m.start():x}, {len(out)} bytes")
            return out
    sys.exit("no initramfs containing usr/sbin/ulcmd found (firmware without the LCD UI?)")


def needed(path):
    out = run(["readelf", "-d", path]).stdout.decode()
    return re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    fw_path, out_path = sys.argv[1:]
    fw = open(fw_path, "rb").read()
    rootfs_cpio = find_rootfs(find_kernel(fw))

    work = tempfile.mkdtemp(prefix="utr-stock-")
    try:
        root = os.path.join(work, "rootfs")
        os.mkdir(root)
        subprocess.run(["cpio", "-idm", "--quiet", "--no-absolute-filenames"], input=rootfs_cpio,
                       cwd=root, stderr=subprocess.DEVNULL)   # absolute /var symlink noise is harmless
        ulcmd = os.path.join(root, "usr/sbin/ulcmd")
        if not os.path.isfile(ulcmd):
            sys.exit("ulcmd missing after extraction")

        bundle = os.path.join(work, "utr-ui-stock")
        os.makedirs(os.path.join(bundle, "bin"))
        os.makedirs(os.path.join(bundle, "lib"))
        shutil.copy(ulcmd, os.path.join(bundle, "bin/ulcmd"))

        search = [os.path.join(root, d) for d in ("lib", "usr/lib")]
        seen, todo = set(), needed(ulcmd)
        while todo:
            lib = todo.pop()
            if lib in seen:
                continue
            seen.add(lib)
            src = next((os.path.join(d, lib) for d in search if os.path.exists(os.path.join(d, lib))), None)
            if not src:
                sys.exit(f"needed library {lib} not found in firmware rootfs")
            shutil.copy(os.path.realpath(src) if not os.path.islink(src) else os.path.join(os.path.dirname(src), os.readlink(src)),
                        os.path.join(bundle, "lib", lib))
            todo += needed(os.path.join(bundle, "lib", lib))
        # the musl loader doubles as libc.so; the rest of the closure is resolved above
        if "libc.so" not in seen:
            sys.exit("libc.so (musl loader) missing from dependency closure")
        for p in os.listdir(os.path.join(bundle, "lib")):
            os.chmod(os.path.join(bundle, "lib", p), 0o755)
        os.chmod(os.path.join(bundle, "bin/ulcmd"), 0o755)

        ver = f"{os.path.basename(fw_path)} sha256:{hashlib.sha256(fw).hexdigest()[:16]}"
        open(os.path.join(bundle, "VERSION"), "w").write(ver + "\n")

        with tarfile.open(out_path, "w:gz") as t:
            t.add(bundle, arcname="utr-ui-stock", filter=lambda i: (setattr(i, "uid", 0) or setattr(i, "gid", 0) or setattr(i, "uname", "") or setattr(i, "gname", "") or i))
        print("libs:", " ".join(sorted(seen)))
        print(f"wrote {out_path} ({os.path.getsize(out_path) // 1024} KiB)")
    finally:
        shutil.rmtree(work, ignore_errors=True)


main()
