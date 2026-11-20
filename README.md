# LineageOS 24.0 (Android 17) for the LG V30 (joan) on a mainline kernel

An unofficial LineageOS 24.0 port for the LG V30 that runs on a mainline-based
**Linux 6.18** kernel instead of LG's 4.4 vendor kernel. Graphics, Wi-Fi,
Bluetooth and the rest use open drivers (Mesa freedreno, ath10k, btqca, ...)
rather than the old Qualcomm/LG HALs.

> **Status: early.** The hardware support below was brought up on LineageOS
> 23.2 (Android 16); this tree is the port of that device tree to LineageOS
> 24.0 (Android 17) and is still being validated. On 23.2 it boots to a usable
> Android 16 and most hardware works,
> including mobile data, Android audio, the cameras and the sensors. SELinux
> is still permissive and some features are missing (see below). Treat it
> as a testing build, not a daily driver.

Tested only on the **US998** (V30+, 128 GB). Other variants share the
hardware, but nobody has tried them.

## What works

| Area | State |
|---|---|
| Boot to Android 16 | Works |
| Display (1440x2880, 60 Hz) | Works |
| GPU | OpenGL ES 3.1 via Mesa freedreno (no Vulkan on A540) |
| Touch, buttons | Works, including after screen off/on |
| Mobile network | LTE data and registration work (tested on one carrier) |
| Wi-Fi | Scans and connects (ath10k, no vendor HAL) |
| Bluetooth | Works (btqca/hci_uart) |
| Audio | Loudspeaker through Android's audio HAL |
| Quad DAC | ES9218P with digital filter, headphone type and analog volume controls, a Quad DAC tile and a 24-bit link (headphone output not yet tested by ear) |
| Cameras | All three: preview and photos (libcamera, software ISP) |
| Flash | Torch from the Quick Settings tile with AOSP's strength slider (long-press), and camera flash |
| Sensors | Accelerometer, gyroscope, magnetometer, barometer, light, proximity |
| USB / ADB | Works; USB debugging is off until enabled in Developer options |
| Battery, charging | Works |
| CPU, GPU and memory scaling | CPU to 2.4576 GHz, GPU to 710 MHz, DDR follows CPU load |
| Swap | 3 GB zram |
| Recovery | LineageOS recovery; ADB sideload works |

## What does not work yet

| Area | State |
|---|---|
| SELinux | Permissive (policy for the new HALs is not written yet) |
| Fingerprint | Not working (the stock HAL needs Qualcomm's secure environment) |
| GPS | No GNSS HAL |
| Camera quality | Software ISP: exposure can oscillate; the 16 MP main rear camera (imx351) is not selectable in the camera app |
| Vulkan | None (Turnip needs a6xx+; the proprietary KGSL route is still WIP) |

## Building

You need a normal LineageOS 24.0 build environment
([LineageOS build guide](https://wiki.lineageos.org/build_guides)).

```sh
mkdir lineage && cd lineage
repo init -u https://github.com/LineageOS/android.git -b lineage-24.0 --git-lfs
mkdir -p .repo/local_manifests
curl -o .repo/local_manifests/joan.xml \
  https://codeberg.org/scada/android_local_manifest_joan/raw/branch/lineage-24.0/joan.xml
repo sync -c -j8

# The joan kernel patches on top of ACK, plus a few fixes outside the
# device tree (recovery, USB, Wi-Fi HAL, Bluetooth, OTA tooling). Re-run
# after every repo sync.
device/lge/joan/apply-patches.sh

source build/envsetup.sh
breakfast joan
m bacon
```

The kernel is the Android Common Kernel `android17-6.18` (Linux 6.18.32),
which the manifest checks out at commit `7626905e035e` into `kernel/lge/joan`,
plus the 80 joan patches in `patches/kernel/lge/joan/`: ShapeShifter499's
mainline V30 port, squashed per subsystem, and the backports and fixes this
port needed. It is built as part of the ROM with LineageOS's clang. The output is `out/target/product/joan/lineage-24.0-*-UNOFFICIAL-joan.zip`,
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
   `adb sideload lineage-24.0-*-UNOFFICIAL-joan.zip`.

Back up your modem, modemst1/2, fsg, fsc and persist partitions before doing
anything. They hold your IMEI and calibration.

## Debugging

- Builds are userdebug with SELinux permissive while the port is in bring-up.
- Release builds keep no logs. Build with `JOAN_DEBUG=true` in the
  environment for a development build: it turns adb on at boot and saves
  the kernel log, logcat and pstore of every boot under
  `/data/local/tmp/joan/` (readable from recovery).
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
| KGSL (experimental) | [kgsl_joan](https://codeberg.org/scada/kgsl_joan), branch `joan-6.18`: Qualcomm's open KGSL ported to the A540 on 6.18 |

## Credits

- ShapeShifter499's mainline Linux port for the V30
  ([linux-lg-v30-joan](https://github.com/ShapeShifter499/linux-lg-v30-joan)),
  which the kernel is based on, and the postmarketOS community.
- The LineageOS joan / joan-common maintainers, for the overlays and
  configuration reused here.
- The Linux, Mesa, libcamera and AOSP projects.
