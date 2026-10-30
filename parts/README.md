# Joan Quick Settings

`JoanDeviceParts` supplies the Quad DAC and flashlight tiles and a Sound
settings entry. The tile icon and translated labels were recovered from the saved official
`lineage-22.2-20261018-nightly-joan-signed.zip` system_ext image.
The Quad DAC tile follows the LineageOS 22 tile's enabled,
disabled and unplugged states, using the same package and service component:
https://github.com/LineageOS/android_hardware_lge/blob/lineage-22.2/dac_panel/src/org/lineageos/settings/device/dac/QuadDACTileService.java

The backend uses `AudioManager` parameters handled by `audio.primary.joan`.
Changing the DAC route puts the frontend in standby under the existing device
and stream locks; the next write reopens it on QUAT MI2S or WCD9340 bypass.
The existing kernel S24_LE/32-bit-slot change is required for Quad DAC playback.
The panel also exposes the driver's digital filter, headphone type and analog
attenuation controls. The default attenuation is -14 dB. Saved mixer settings
are restored by a job after the delayed ADSP sound card appears.

The flashlight uses the PMI8998 LED's continuous `brightness` interface,
not flash strobe current. Five steps use 20/40/60/80/100% of `max_brightness`;
the DT caps continuous current at 200 mA. Tap toggles the last selected step;
hold opens the step selector. Only the chosen brightness persists across boot.
This device tile works independently of the camera provider, which currently
does not implement the Android torch API. Camera apps and the framework's
built-in flashlight tile do not gain torch support from this change.

New installs get both tiles by default. A one-time boot migration replaces
the framework flashlight tile and adds Quad DAC, preserving other user tiles.
The torch requires the scoped `sysfs_joan_torch` label and the existing
ueventd system ownership of LED brightness. Confirm the actual device path
with `readlink -f /sys/class/leds/white:flash` on the phone.

Build and hardware validation are separate. Host command:

```sh
bash ~/android/build-flash-tiles.sh
```

It builds the current complete source tree, validates the staged OTA and image
checksums, checks the tiles and HAL are fresh, saves the installed boot image,
sideloads through recovery, and collects device logs. It does not wipe data.
Use `build-only` as the argument to stop after image validation.

On-device acceptance checks: wired headphones make Quad DAC available;
toggling while playing switches routes without a hang; QUAT playback reports
S24_LE at 48 kHz; all five torch levels visibly differ; toggling remembers the
selected level; reboot leaves the torch off and restores DAC preferences.
The camera must be tested separately before adding torch support there.
