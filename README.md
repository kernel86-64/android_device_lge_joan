# LineageOS 24.0 (Android 17) for the LG V30 (joan) on a mainline kernel

An unofficial LineageOS 24.0 port for the LG V30 (H930/H930DS, MSM8998) that
runs on a mainline-based **Linux 7.3** kernel instead of LG's 4.4 vendor
kernel. The display, Wi-Fi, the cellular modem, audio, the cameras and the
torch use open drivers (MSM DRM, ath10k, qcom SMEM/QRTR, libcamera, ...)
rather than the old Qualcomm/LG HALs. The Adreno 540 can additionally run on
Qualcomm's open KGSL driver on top of the same kernel.

> **Status: testing build.** The port was brought up on LineageOS 23.2
> (Android 16) with Linux 6.18 and now builds on LineageOS 24.0 (Android 17)
> with Linux 7.3. It boots to a usable Android 17 and nearly all hardware
> works: display and Always-On Display, touch, Wi-Fi on 2.4 and 5 GHz, the
> cellular modem (baseband, IMEI, network registration), audio and the
> ES9218P Quad DAC, all three cameras, the torch with the Quick Settings
> strength slider, the sensors, haptics, battery/charging and ADB. SELinux is
> still permissive, and fingerprint, GPS, VoLTE validation and USB Type-C
> role switching are missing (see below). Treat it as a testing build, not a
> daily driver.

Tested only on the **H930DS**. Other variants share the hardware, but nobody
has tried them.

## What works

| Area | State |
|---|---|
| Boot to Android 17 | Works |
| Display | 1440x2880 at 60 Hz, brightness, Always-On Display (off by default) |
| GPU | Adreno 540 through the out-of-tree KGSL stack: OpenGL ES 3.2 (`V@0490.0`) and Vulkan 1.1.128, including six consecutive sleep/wake/unlock cycles. The in-tree Mesa freedreno path provides OpenGL ES 3.1 without Vulkan |
| Touch, buttons | Works, including after screen off/on |
| Modem | MPSS starts and stays running (no watchdog or PS_HOLD resets), baseband and IMEI are read from the phone, the modem registers on the network (`Emergency calls only` without a SIM). VoLTE is not tested |
| Wi-Fi | ath10k/WCN3990: scans 2.4 and 5 GHz, connects, gets an address and passes traffic |
| Bluetooth | Works (btqca/hci_uart); `hci0: Frame reassembly failed (-84)` still appears in dmesg |
| Audio | ALSA card `LG-V30` with 7 PCM devices, TFA9872 speaker amplifier |
| Quad DAC | ES9218P with digital filter, headphone type and analog volume controls, a Quad DAC tile and a 24-bit link (headphone output not yet tested by ear) |
| Cameras | All three cameras: preview and photos (libcamera + software ISP on CAMSS) |
| Flash | Torch from the Quick Settings tile with AOSP's strength slider (long-press), and camera flash |
| Sensors | Accelerometer, gyroscope, magnetometer, barometer, light, proximity |
| Vibration | DW7800 haptics with Android effects |
| Battery, charging | PMI8998 fuel gauge via `qcom-battery`; charging and 3 GB zram swap |
| CPU, GPU and memory scaling | CPU to 2.4576 GHz, GPU to 710 MHz, DDR follows CPU load through BWMON/OPP |
| USB / ADB | Configfs gadgets; USB debugging is off until enabled in Developer options |
| Recovery | LineageOS recovery; ADB sideload works |

## What does not work yet

| Area | State |
|---|---|
| SELinux | Permissive (policy for the new HALs is not written yet) |
| Fingerprint | Not working (the stock HAL needs Qualcomm's secure environment) |
| GPS | No GNSS HAL |
| VoLTE | Not validated: the modem registers and data attaches, but the IMS/VoLTE path is untested |
| USB Type-C | Role and alt-mode switching not ported (`qcom_smbx` / PMIC Type-C are missing) |
| Camera quality | Software ISP: exposure can oscillate; the 16 MP main rear camera (imx351) is not selectable in the camera app |
| Vulkan (Mesa) | None (Turnip needs a6xx+); Vulkan is available only through KGSL |

## GPU: KGSL and Mesa

The Adreno 540 is driven by Qualcomm's open KGSL driver, built out of tree
from [kgsl_joan](https://github.com/kernel86-64/kgsl_joan) (branch `joan-7.3`)
with [ion_legacy](https://github.com/kernel86-64/ion_legacy) providing the
64-bit `/dev/ion` ABI over the mainline dma-buf heaps. It runs with
`msm.separate_gpu_kms=1`, so DRM owns the display and KGSL owns the GPU:
OpenGL ES 3.2 (V@0490.0) and Vulkan 1.1.128 work, and the RGB565 scanout
colours are fixed in `graphics/BufferInfoQcom.cpp`.

The device-specific kernel changes are in the KGSL tree: skip the 27 MHz
slumber OPP that the mainline msm8998 DT carries (it has no DDR bandwidth),
never touch per-context SMMU registers once the global registers are secured
(otherwise the XPU resets the SoC after the zap shader), and clear
`ADRENO_DEVICE_GPMU_INITIALIZED` before the RBBM software reset so the GPMU
is re-initialised.

This stack is experimental and currently deployed outside the default ROM
image. A plain build from this device tree uses the Mesa freedreno path
(OpenGL ES 3.1, no Vulkan); the KGSL modules and userspace pieces are not yet
wired into the product makefiles.

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

The manifest pulls `device/lge/joan` and `vendor/lge/joan` from
github.com/kernel86-64 and pins the projects that `apply-patches.sh` patches
(`build/make`, `build/soong`, `bootable/recovery`, `frameworks/base`,
`external/drm_hwcomposer`, `frameworks/opt/net/wifi`, `hardware/interfaces`,
`device/lineage/sepolicy`, `hardware/lineage/interfaces`,
`packages/apps/Aperture`) to the revisions their diffs were generated
against, so the tree stays reproducible as `lineage-24.0` moves.

Subsequent builds must reuse the existing `out` and ccache. Do not run
`clean`, `clobber`, `installclean`, or remove build state unless the user
explicitly requests it. Use the same SISO build path for builds and tests;
do not run standalone Ninja against this `out`.

The kernel is the Android Common Kernel `android-mainline-7.3` (the v7.3
merge, `d9ee36f9bbc0`, pinned by the manifest into `kernel/lge/joan-7.3`),
plus the joan patch series in `patches/kernel/lge/joan-7.3/`: ShapeShifter499's
mainline V30 port, squashed per subsystem, and the fixes this port needed.
`apply-patches.sh` applies the series (59 patches, `BASE`/`TREE` checked) and
the kernel is built as part of the ROM with LineageOS's clang. The output is
`out/target/product/joan/lineage-24.0-*-UNOFFICIAL-joan.zip`, together with
`boot.img` and `recovery.img`.

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
Keep the device's matching H930DS firmware partition installed: a vendor
image must not replace it with a US998 set. The vendor image includes the
mountpoint and the AOSP supplicant configuration template.

After filesystem services and Android are ready, `joan_rprocs.sh` starts
ADSP/audio, registers ath10k, then starts the modem. The modem boot depends
on the MMSS QoS-monitor clocks staying enabled (`gcc_mmss_qm_ahb_clk` and
`gcc_mmss_qm_core_clk` are marked `CLK_IS_CRITICAL`), on the userspace
`pd-mapper` for the ADSP/SLPI/modern protection domains, and on `rmtfs`
being able to write the modem's EFS. Startup does not bind-mount firmware,
preassign MSA memory or force remoteproc crashes/retries.

The 7.3 build was verified on an H930DS using the phone's own firmware
partition: MPSS runs without resets, Wi-Fi scans, connects and passes
traffic, the modem registers on the network, the three cameras and the
torch, audio and the sensors work. VoLTE, mobile data throughput and
suspend/resume still need testing.

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
- The modem, ADSP and SLPI states are visible in
  `/sys/class/remoteproc/remoteproc*/state`; `rmtfs` and `pd-mapper` must be
  running before `joan_rprocs.sh` starts the modem.
- If you put your ADB public key in `device/lge/joan/adb_keys` before
  building (it is gitignored), recovery and Android trust your machine
  without tapping Allow, even after a data wipe.

## Repositories

| Path | Repository |
|---|---|
| `device/lge/joan` | [android_device_lge_joan](https://github.com/kernel86-64/android_device_lge_joan) |
| `kernel/lge/joan-7.3` | ACK [kernel/common](https://android.googlesource.com/kernel/common) at `d9ee36f9bbc0` + `patches/kernel/lge/joan-7.3/` |
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
