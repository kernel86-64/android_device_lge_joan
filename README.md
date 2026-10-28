# LineageOS 23.2 (Android 16) for the LG V30 (joan) on a mainline kernel

An unofficial LineageOS 23.2 port for the LG V30 that runs on a mainline-based
**Linux 6.18** kernel instead of LG's 4.4 vendor kernel. Graphics, Wi-Fi,
Bluetooth and the rest use open drivers (Mesa freedreno, ath10k, btqca, ...)
rather than the old Qualcomm/LG HALs.

> **Status: early bring-up.** It boots to a usable Android 16 home screen, but
> several core features (mobile network, camera preview, Android audio) do not work
> yet. Do not use it as a daily driver.

Tested only on the **US998** (V30+, 128 GB). Other variants share the
hardware, but nobody has tried them.

## What works

| Area | State |
|---|---|
| Boot to Android 16 | Works |
| Display (1440x2880, 60 Hz) | Works |
| GPU | OpenGL ES via Mesa freedreno (no Vulkan on A540) |
| Touch, buttons | Works, including after screen off/on |
| Wi-Fi | Scans and connects (ath10k, no vendor HAL) |
| Bluetooth | Works (btqca/hci_uart) |
| USB / ADB | Works; MTP needs the framework running |
| Battery, charging | Works |
| CPU and GPU frequency scaling | Works (CPU to 2.4576 GHz, GPU to 710 MHz) |
| Recovery | LineageOS recovery; ADB sideload works |
| Sound card | ADSP, SLIMbus and WCD934x come up; loudspeaker plays via ALSA (`tinyplay`) |

## What does not work yet

| Area | State |
|---|---|
| Audio in Android | No audio HAL wired to the new sound card yet |
| Mobile network / calls | Modem crash-loops; RIL never registers |
| Camera | All three cameras enumerate (libcamera, software ISP), preview fails |
| Fingerprint | Not working |
| Sensors | No sensors HAL |
| Smoothness | Notification shade drops many frames at native resolution |
| Vulkan | None (Turnip needs a6xx+; the proprietary KGSL route is still WIP) |

## Building

You need a normal LineageOS 23.2 build environment
([LineageOS build guide](https://wiki.lineageos.org/build_guides)).

```sh
mkdir lineage && cd lineage
repo init -u https://github.com/LineageOS/android.git -b lineage-23.2 --git-lfs
mkdir -p .repo/local_manifests
curl -o .repo/local_manifests/joan.xml \
  https://codeberg.org/scada/android_local_manifest_joan/raw/branch/lineage-23.2/joan.xml
repo sync -c -j8

# The joan kernel patches on top of ACK, plus a few fixes outside the
# device tree (recovery, USB, Wi-Fi HAL, Bluetooth, libcutils memfd, OTA
# tooling). Re-run after every repo sync.
device/lge/joan/apply-patches.sh

source build/envsetup.sh
breakfast joan
m bacon
```

The kernel is the Android Common Kernel `android17-6.18` (Linux 6.18.32),
which the manifest checks out at commit `7626905e035e` into `kernel/lge/joan`,
plus the 54 joan patches in `patches/kernel/lge/joan/`: ShapeShifter499's
mainline V30 port, squashed per subsystem, and the backports and fixes this
port needed. It is built as part of the ROM with LineageOS's clang. The output is `out/target/product/joan/lineage-23.2-*-UNOFFICIAL-joan.zip`,
together with `boot.img` and `recovery.img`.

## Installing

1. Unlock the bootloader and install the official
   **LineageOS 22.2** for joan first. It provides the firmware partitions
   (modem, DSPs) this build expects.
2. Flash this build's `recovery.img` to the recovery partition. LG's
   bootloader only accepts `fastboot flash` when fastboot runs as root
   (`sudo fastboot flash recovery recovery.img`); from a rooted system,
   `dd` to `/dev/block/by-name/recovery` works too.
3. Boot recovery (from LineageOS 22.2: `adb reboot recovery`), choose
   **Factory reset -> Format data** (LineageOS 22.2's ICE encryption is
   not compatible), then **Apply update -> ADB sideload** and run
   `adb sideload lineage-23.2-*-UNOFFICIAL-joan.zip`.

Back up your modem, modemst1/2, fsg, fsc and persist partitions before doing
anything. They hold your IMEI and calibration.

## Debugging

- Builds are userdebug with SELinux permissive while the port is in bring-up.
- `androidboot.joan.bootlog=1` on the kernel command line saves dmesg and
  logcat of each boot to `/cache/joan/`.
- If you put your ADB public key in `device/lge/joan/adb_keys` before
  building (it is gitignored), recovery and Android trust your machine
  without tapping Allow, even after a data wipe.

## Repositories

| Path | Repository |
|---|---|
| `device/lge/joan` | [android_device_lge_joan](https://codeberg.org/scada/android_device_lge_joan) |
| `kernel/lge/joan` | ACK [kernel/common](https://android.googlesource.com/kernel/common) at `7626905e035e` + `patches/kernel/lge/joan/` |
| `vendor/lge/joan` | [proprietary_vendor_lge_joan](https://codeberg.org/scada/proprietary_vendor_lge_joan) |
| libcamera prebuilts | built from [libcamera_joan](https://codeberg.org/scada/libcamera_joan), branch `joan-android` |

## Credits

- ShapeShifter499's mainline Linux port for the V30
  ([linux-lg-v30-joan](https://github.com/ShapeShifter499/linux-lg-v30-joan)),
  which the kernel is based on, and the postmarketOS community.
- The LineageOS joan / joan-common maintainers, for the overlays and
  configuration reused here.
- The Linux, Mesa, libcamera and AOSP projects.
