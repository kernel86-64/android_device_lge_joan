#!/usr/bin/env python3
"""Package a compiled recovery with the last known bootable joan kernel.

This does not flash anything. It uses the existing recovery staging tree and
checks the sideload patch and embedded public key before producing an image.
"""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def cpio_files(data):
    files = {}
    pos = 0
    while pos + 110 <= len(data):
        header = data[pos:pos + 110]
        if header[:6] not in (b"070701", b"070702"):
            raise ValueError("Invalid newc archive")
        size = int(header[54:62], 16)
        namesize = int(header[94:102], 16)
        name = data[pos + 110:pos + 110 + namesize - 1].decode()
        pos = (pos + 110 + namesize + 3) & ~3
        if name == "TRAILER!!!":
            return files
        files[name.removeprefix("./")] = data[pos:pos + size]
        pos = (pos + size + 3) & ~3
    raise ValueError("Missing cpio trailer")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--top", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    top = args.top.resolve()
    out = top / "out/target/product/joan"
    host = top / "out/host/linux-x86/bin"
    device = top / "device/lge/joan"
    output = args.output.resolve()
    if output.exists():
        raise SystemExit(f"Refusing to overwrite {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="joan-recovery-", dir=output.parent) as temp:
        temp = Path(temp)
        root = temp / "root"
        shutil.copytree(out / "recovery/root", root, symlinks=True)
        (root / "adb_keys").unlink(missing_ok=True)
        shutil.copyfile(device / "adb_keys", root / "adb_keys")
        shutil.copyfile(device / "rootdir/etc/init.recovery.joan.rc",
                        root / "init.recovery.joan.rc")
        rc = (root / "init.recovery.joan.rc").read_bytes()
        binary = (root / "system/bin/recovery").read_bytes()
        assert b"setprop debug.recovery.auto_sideload 1" in rc
        assert b"setprop debug.recovery.auto_reboot 1" in rc
        assert b"Starting automatic ADB sideload" in binary, "Compile recovery first"
        assert b"ro.debuggable=1\n" in (root / "prop.default").read_bytes()
        assert (root / "adb_keys").read_bytes().strip(), "Missing host public key"
        archive = subprocess.check_output([host / "mkbootfs", "-d", out / "system", root])
        files = cpio_files(archive)
        assert files["adb_keys"] == (device / "adb_keys").read_bytes()
        assert files["system/bin/recovery"] == binary
        ramdisk = temp / "ramdisk.gz"
        ramdisk.write_bytes(gzip.compress(archive, compresslevel=9, mtime=0))

        # Preserve the tested kernel, offsets, header version and appended DTB.
        raw_args = subprocess.check_output([
            host / "unpack_bootimg", "--boot_img", out / "recovery.img",
            "--out", temp / "baseline", "--format", "mkbootimg", "-0"])
        # Empty argument values (in particular --board) are significant.
        boot_args = raw_args.decode().split("\0")
        if boot_args[-1] == "":
            boot_args.pop()
        boot_args[boot_args.index("--ramdisk") + 1] = str(ramdisk)
        cmd = boot_args.index("--cmdline") + 1
        boot_args[cmd] = boot_args[cmd].replace("ignore_loglevel ", "").replace(
            "loglevel=8", "loglevel=4").replace("androidboot.joan.logcat_kmsg=1",
                                                "androidboot.joan.logcat_kmsg=0")
        subprocess.run([host / "mkbootimg", *boot_args, "--output", output], check=True)
        assert output.stat().st_size < 41943040, "Recovery exceeds partition size"
        # Verify the actual image, including its compressed ramdisk.
        subprocess.run([host / "unpack_bootimg", "--boot_img", output,
                        "--out", temp / "verify"], check=True, stdout=subprocess.DEVNULL)
        packed = cpio_files(gzip.decompress((temp / "verify/ramdisk").read_bytes()))
        assert packed["adb_keys"] == files["adb_keys"]
        assert packed["init.recovery.joan.rc"] == rc
        assert packed["system/bin/recovery"] == binary
        sha = hashlib.sha256(output.read_bytes()).hexdigest()
        output.with_suffix(".img.sha256").write_text(f"{sha}  {output.name}\n")
        report = {
            "image": str(output), "sha256": sha, "bytes": output.stat().st_size,
            "kernel": "unchanged from existing out/target/product/joan/recovery.img",
            "automatic_sideload": True, "reboot_after_success_only": True,
            "adb_authentication": "embedded build-host public key",
            "hardware_tested": False,
        }
        output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
