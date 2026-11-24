# LineageOS 24.0 (Android 17) for the LG V30 (joan) on a mainline kernel

An unofficial LineageOS 24.0 port for the LG V30 that runs on a mainline-based
**Linux 7.3** kernel instead of LG's 4.4 vendor kernel. Graphics, Wi-Fi,
Bluetooth and the rest use open drivers (Mesa freedreno, ath10k, btqca, ...)
rather than the old Qualcomm/LG HALs.

> **Status: early.** The port was brought up on LineageOS 23.2 (Android 16)
> with Linux 6.18 and now builds on LineageOS 24.0 (Android 17) with Linux 7.3.
> It boots to a usable Android 17: display, Wi-Fi, the cellular modem, audio,
> all three cameras, the torch and the sensors work. SELinux is still
> permissive and some features are missing (see below). Treat it as a testing
> build, not a daily driver.

Tested only on the **H930DS**. Other variants share the hardware, but nobody
has tried them.

## What works

| Area | State |
|---|---|
| Boot to Android 17 | Works |
| Display (1440x2880, 60 Hz) | Works |
| GPU | OpenGL ES 3.1 via Mesa freedreno; OpenGL ES 3.2 and Vulkan 1.1 with the experimental KGSL/Adreno stack |
| Touch, buttons | Works, including after screen off/on |
| Mobile network | The modem initialises and reaches the network; SIM registration is still under test |
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
| Vulkan | 1.1 with the experimental out-of-tree KGSL stack; none with Mesa (Turnip needs a6xx+) |

## Building

You need a normal LineageOS 24.0 build environment
([LineageOS build guide](https://wiki.lineageos.org/build_guides)).

```sh
mkdir lineage && cd lineage
repo init -u https://github.com/LineageOS/android.git -b lineage-24.0 --git-lfs
mkdir -p .repo/local_manifests
curl -o .repo/local_manifests/joan.xml \
  https://raw.githubusercontent.com/kernel86-64/android_device_lge_joan/lineage-24.0/local_manifest.xml
repo sync -c -j8

# The joan kernel patches on top of ACK, plus a few fixes outside the
# device tree (recovery, USB, Wi-Fi HAL, Bluetooth, OTA tooling). Re-run
# after every repo sync.
device/lge/joan/apply-patches.sh

source build/envsetup.sh
breakfast joan
m bacon
```

Subsequent builds must reuse the existing `out` and ccache. Do not run
`clean`, `clobber`, `installclean`, or remove build state unless the user
explicitly requests it. Use the same SISO build path for builds and tests;
do not run standalone Ninja against this `out`.

The kernel is the Android Common Kernel `android-mainline-7.3` (the v7.3
merge, `d9ee36f9bbc0`, pinned by the manifest into `kernel/lge/joan-7.3`),
plus the joan patch series in `patches/kernel/lge/joan-7.3/`: ShapeShifter499's
mainline V30 port, squashed per subsystem, and the fixes this port needed. The
manifest also keeps the 6.18.32 project in `kernel/lge/joan` with its own
series for rollback. `apply-patches.sh` applies whichever series is present,
and the kernel is built as part of the ROM with LineageOS's clang. The output is
`out/target/product/joan/lineage-24.0-*-UNOFFICIAL-joan.zip`,
together with `boot.img` and `recovery.img`.

The RPM interconnect caches acknowledged average-bandwidth votes by node
and context. This removes redundant synchronous RPM messages from CPU OPP
changes. Initial zero votes and retries after failed writes are preserved;
provider reprobe clears the cache validity.

`power/powerhint.json` also enables standard ADPF CPU hint sessions. HWUI
and SurfaceFlinger report their frame work durations, and the Power HAL
adjusts per-thread uclamp requests against the target duration. Stale votes
expire during inactivity. Both clients are enabled in `device.mk`; no
private vendor scheduler interface is required for the CPU hint path.

On the tested H930DS, `fastboot boot` omitted the usual LG boot parameters
and reported `LG-H930` instead of `LG-H930DS`; the modem then rejected its
model information. Normal boot restored the H930DS parameters and working
Wi-Fi/modem. Check `ro.boot.vendor.lge.model.name` when comparing boot paths.

### Brightness

Android uses a positive linear minimum of `0.0002`. The upstream HWC HLG
conversion maps it to sysfs level 6, which the panel table maps to DBV 10
(the low level checked on H930DS). Keep that transfer function: removing it
distorts the middle of Android's perceptual brightness slider. Maximum
brightness remains 255 and display-off is handled separately.

The stock physical-nits calibration is disabled because it describes a
different backlight pipeline. `configs/displayconfig/display_port_0.xml`
stores floating-point lux/linear-brightness targets, converted from the
former backlight targets with the inverse HLG function. This avoids the
quantization of low levels in the legacy 8-bit automatic-brightness table.
Measuring the current panel pipeline is required before enabling physical
nits calibration again.

### Firmware selection

The read-only `modem` partition is mounted at `/vendor/firmware_mnt`.
Remoteproc firmware, `modem_pr` carrier configuration, WLAN DSP firmware,
WLAN board data and IPA firmware are linked to its `image/` directory.
Keep the device's matching firmware partition installed: a vendor image
must not replace H930DS firmware with the US998 set. The vendor image
includes the mountpoint and the AOSP supplicant configuration template.

After filesystem services and Android are ready, `joan_rprocs.sh` starts
ADSP/audio, registers ath10k, then starts the modem. It publishes WLAN
readiness when `wlan0` appears. Startup does not bind-mount firmware,
preassign MSA memory or force remoteproc crashes/retries.

The 7.3 build was verified on an H930DS using the phone's own firmware
partition: Wi-Fi scans, connects and passes traffic, the modem starts and
reaches the network, the three cameras and the torch, audio and the sensors
work. SIM registration, mobile data and suspend/resume still need testing.

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
| `device/lge/joan` | [android_device_lge_joan](https://github.com/kernel86-64/android_device_lge_joan) |
| `kernel/lge/joan-7.3` | ACK [kernel/common](https://android.googlesource.com/kernel/common) at `d9ee36f9bbc0` + `patches/kernel/lge/joan-7.3/` |
| `kernel/lge/joan` | ACK kernel/common at `7626905e035e` + `patches/kernel/lge/joan/` (6.18 rollback) |
| `vendor/lge/joan` | [proprietary_vendor_lge_joan](https://github.com/kernel86-64/proprietary_vendor_lge_joan) |
| libcamera prebuilts | in-tree `prebuilts/libcamera`, built from libcamera with the joan pipeline |
| KGSL / ION (experimental) | [kgsl_joan](https://github.com/kernel86-64/kgsl_joan) (branch `joan-7.3`) and [ion_legacy](https://github.com/kernel86-64/ion_legacy) |

## Credits

- ShapeShifter499's mainline Linux port for the V30
  ([linux-lg-v30-joan](https://github.com/ShapeShifter499/linux-lg-v30-joan)),
  which the kernel is based on, and the postmarketOS community.
- The LineageOS joan / joan-common maintainers, for the overlays and
  configuration reused here.
- The Linux, Mesa, libcamera and AOSP projects.
