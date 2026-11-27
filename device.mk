#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# LG V30 (joan) on a mainline-based kernel: no LG/Qualcomm userspace blobs,
# only upstream drivers plus open HALs (Mesa freedreno, drm_hwcomposer,
# minigbm, AOSP default HALs) and the extracted firmware.
#

LOCAL_PATH := device/lge/joan

PRODUCT_SHIPPING_API_LEVEL := 25

# ART: use the CC collector. With CMC (userfaultfd) zygote hangs right after
# "Using CollectorTypeCMC GC" on this kernel; revisit once that is understood.
PRODUCT_ENABLE_UFFD_GC := false
PRODUCT_USE_DYNAMIC_PARTITIONS := true

PRODUCT_SOONG_NAMESPACES += $(LOCAL_PATH)

# aconfig flag overrides for the cp2a release (QS flashlight strength slider)
PRODUCT_RELEASE_CONFIG_MAPS += $(LOCAL_PATH)/release/release_config_map.textproto

# Sensors: the SLPI serves them through the qcom-smgr IIO driver; the
# joan IIO sub-HAL runs under the AIDL multihal. sns-reg answers the
# SLPI's registry requests from LG's sensor_def files, without which the
# SLPI never starts its Sensor Manager.
PRODUCT_PACKAGES += \
    android.hardware.sensors-service.multihal \
    android.hardware.sensors@2.1-joan-iio-subhal \
    sns-reg
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/sensors/sensor_def_common.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/sensor_def_common.conf \
    $(LOCAL_PATH)/configs/sensors/sensor_def_joan_global_com.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/sensor_def_variable.conf \
    $(LOCAL_PATH)/sensors/config/hals.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/hals.conf \
    $(LOCAL_PATH)/sensors/config/sensor_hal_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/sensor_hal_configuration.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.barometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.barometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.proximity.xml

# Overlays (from the LineageOS 22.2 joan tree, minus what needs the
# downstream vendor stack: light sensor, doze sensors, QTI telephony, the
# vendor Wi-Fi HAL)
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay \
    $(LOCAL_PATH)/overlay-lineage
PRODUCT_ENFORCE_RRO_TARGETS := *
PRODUCT_ENFORCE_RRO_EXCLUDED_OVERLAYS += $(LOCAL_PATH)/overlay-lineage/lineage-sdk
PRODUCT_PACKAGES += WifiOverlay.joan

# Bring-up debugging: trust the build host's adb key, adb on from boot
ifneq ($(wildcard $(LOCAL_PATH)/adb_keys),)
PRODUCT_ADB_KEYS := $(LOCAL_PATH)/adb_keys
# adbd only reads /adb_keys and /data/misc/adb/adb_keys, but PRODUCT_ADB_KEYS
# lands in /product/etc/security/adb_keys. Put it in the boot ramdisk too, so a
# wiped /data (which drops the user keys) does not leave adb unauthorized with
# no way to tap Allow in recovery.
PRODUCT_COPY_FILES += $(LOCAL_PATH)/adb_keys:$(TARGET_COPY_OUT_RAMDISK)/adb_keys
# adb only (no MTP) while bringing up: MTP's function is served from
# system_server, and the gadget cannot start until every function is ready.
# Development builds (JOAN_DEBUG=true) enable adb at boot; release builds
# leave USB debugging off until it is turned on in Developer options.
ifeq ($(JOAN_DEBUG),true)
PRODUCT_SYSTEM_DEFAULT_PROPERTIES += persist.sys.usb.config=adb
endif
endif

# Dalvik heap (4 GB RAM); without it ART's 16 MB default makes
# system_server run out of heap during boot.
$(call inherit-product, frameworks/native/build/phone-xhdpi-4096-dalvik-heap.mk)

# Screen
TARGET_SCREEN_HEIGHT := 2880
TARGET_SCREEN_WIDTH := 1440
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := 560dpi
PRODUCT_AAPT_PREBUILT_DPI := xxxhdpi xxhdpi xhdpi hdpi
PRODUCT_VENDOR_PROPERTIES += ro.sf.lcd_density=560

# Boot animation
TARGET_BOOTANIMATION_HALF_RES := true

# Init / fstab / ueventd
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/etc/fstab.joan:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.joan \
    $(LOCAL_PATH)/rootdir/etc/fstab.joan:$(TARGET_COPY_OUT_RAMDISK)/fstab.joan \
    $(LOCAL_PATH)/rootdir/etc/init.joan.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.joan.rc \
    $(LOCAL_PATH)/rootdir/etc/init.joan.usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.joan.usb.rc \
    $(LOCAL_PATH)/rootdir/etc/init.joan.usb.configfs.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.joan.usb.configfs.rc \
    $(LOCAL_PATH)/rootdir/etc/init.joan.radio.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.joan.radio.rc \
    $(LOCAL_PATH)/rootdir/etc/ueventd.joan.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc \
    $(LOCAL_PATH)/rootdir/etc/init.recovery.joan.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.joan.rc \
    $(LOCAL_PATH)/rootdir/bin/joan_rprocs.sh:$(TARGET_COPY_OUT_VENDOR)/bin/joan_rprocs.sh \
    $(LOCAL_PATH)/rootdir/bin/joan_bootlog.sh:$(TARGET_COPY_OUT_VENDOR)/bin/joan_bootlog.sh \
    $(LOCAL_PATH)/rootdir/bin/joan_zram.sh:$(TARGET_COPY_OUT_VENDOR)/bin/joan_zram.sh \
    $(LOCAL_PATH)/rootdir/bin/joan_dyndbg.sh:$(TARGET_COPY_OUT_VENDOR)/bin/joan_dyndbg.sh

# Releasetools (H932 zap shader selection)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/releasetools/device_check.sh:install/bin/device_check.sh

# Recovery: LG keeps the bootloader message 128 bytes into misc.
$(call soong_config_set,lineage_recovery,bootloader_message_offset,128)

# Floating-point panel brightness and automatic-brightness mapping.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/displayconfig/display_port_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_port_0.xml

# Graphics: Mesa freedreno (prebuilt, see prebuilts/mesa), minigbm, drm_hwcomposer
$(call soong_config_set,minigbm,platform,msm)
PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator-service.minigbm \
    mapper.minigbm \
    gralloc.minigbm \
    com.android.hardware.graphics.composer.drm_hwcomposer

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/prebuilts/mesa/lib64/libEGL_mesa.so:$(TARGET_COPY_OUT_VENDOR)/lib64/egl/libEGL_mesa.so \
    $(LOCAL_PATH)/prebuilts/mesa/lib64/libGLESv1_CM_mesa.so:$(TARGET_COPY_OUT_VENDOR)/lib64/egl/libGLESv1_CM_mesa.so \
    $(LOCAL_PATH)/prebuilts/mesa/lib64/libGLESv2_mesa.so:$(TARGET_COPY_OUT_VENDOR)/lib64/egl/libGLESv2_mesa.so \
    $(LOCAL_PATH)/prebuilts/mesa/lib64/libgallium_dri.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libgallium_dri.so \
    frameworks/native/data/etc/android.hardware.opengles.aep.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.opengles.aep.xml

PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.egl=mesa \
    ro.hardware.gralloc=minigbm \
    ro.hardware.hwcomposer=drm \
    ro.opengles.version=196610 \
    debug.hwui.renderer=skiagl \
    debug.renderengine.backend=skiaglthreaded \
    ro.surface_flinger.max_frame_buffer_acquired_buffers=3

# Report UI/compositor frame deadlines through the Power HAL's ADPF sessions.
PRODUCT_SYSTEM_PROPERTIES += \
    debug.hwui.use_hint_manager=true \
    debug.sf.enable_adpf_cpu_hint=true

# Linear buffers only for now: with UBWC render targets on scanout the
# panel stays black (fbcon's linear buffer works). Revisit UBWC later.
PRODUCT_VENDOR_PROPERTIES += vendor.minigbm.debug=nocompression

# No vendor ashmem driver: the kernel builds the memfd-backed ashmem shim,
# and Android 17's libcutils takes sys.use_memfd=true directly.
PRODUCT_SYSTEM_PROPERTIES += sys.use_memfd=true

# No Vulkan on Adreno 5xx with Mesa (turnip is a6xx+).
PRODUCT_VENDOR_PROPERTIES += ro.hardware.vulkan=none

# Health (battery from the PMI8998 fuel gauge via power_supply)
PRODUCT_PACKAGES += \
    android.hardware.health-service.example \
    android.hardware.health-service.example_recovery

# USB port HAL. Gadget functions use UsbHandlerLegacy and our configfs rules;
# the AIDL example gadget service is a stub that never applies functions.
PRODUCT_PACKAGES += \
    android.hardware.usb-service.example \
    joan_adb_suspend

# Keymint / Gatekeeper (software for now; no QSEE on mainline)
PRODUCT_PACKAGES += \
    android.hardware.security.keymint-service \
    com.android.hardware.gatekeeper.nonsecure

# Power / Thermal / Vibrator
PRODUCT_PACKAGES += \
    android.hardware.power-service.lineage-libperfmgr \
    com.android.hardware.thermal \
    android.hardware.vibrator-service.joan

# Lights (panel backlight)
PRODUCT_PACKAGES += android.hardware.light-service.lineage

# Audio: the legacy joan HAL (audio/hal) behind the HIDL 7.0 service, as on
# db845c: routes come from mixer_paths.xml through libaudioroute, which the
# AIDL default HAL has no equivalent for. The same service hosts the AIDL
# Bluetooth audio provider.
PRODUCT_PACKAGES += \
    android.hardware.audio.service \
    android.hardware.audio@7.0-impl \
    android.hardware.audio.effect@7.0-impl \
    android.hardware.bluetooth.audio-impl \
    audio.primary.joan \
    audio.bluetooth.default \
    audio.r_submix.default \
    audio.usb.default \
    libaudiopreprocessing \
    libbundlewrapper \
    libdownmix \
    libdynproc \
    libeffectproxy \
    libhapticgenerator \
    libldnhncr \
    libreverbwrapper \
    libvisualizer \
    tinycap \
    tinymix \
    tinypcminfo \
    tinyplay
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/mixer_paths.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_paths.xml \
    frameworks/av/media/libeffects/data/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects.xml
AUDIO_POLICY_CONFIG_DIR := frameworks/av/services/audiopolicy/config
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    $(AUDIO_POLICY_CONFIG_DIR)/bluetooth_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_audio_policy_configuration_7_0.xml \
    $(AUDIO_POLICY_CONFIG_DIR)/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    $(AUDIO_POLICY_CONFIG_DIR)/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    $(AUDIO_POLICY_CONFIG_DIR)/usb_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usb_audio_policy_configuration.xml \
    $(AUDIO_POLICY_CONFIG_DIR)/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    $(AUDIO_POLICY_CONFIG_DIR)/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    $(AUDIO_POLICY_CONFIG_DIR)/surround_sound_configuration_5_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/surround_sound_configuration_5_0.xml
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.audio.output.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.audio.output.xml

# Bluetooth (Linux HCI)
PRODUCT_PACKAGES += \
    android.hardware.bluetooth-service.default \
    joan_btaddr \
    joan_timekeep
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml
PRODUCT_PRODUCT_PROPERTIES += \
    bluetooth.device.class_of_device=90,2,12 \
    bluetooth.profile.a2dp.source.enabled=true \
    bluetooth.profile.asha.central.enabled=true \
    bluetooth.profile.avrcp.target.enabled=true \
    bluetooth.profile.bas.client.enabled=true \
    bluetooth.profile.gatt.enabled=true \
    bluetooth.profile.hfp.ag.enabled=true \
    bluetooth.profile.hid.device.enabled=true \
    bluetooth.profile.hid.host.enabled=true \
    bluetooth.profile.map.server.enabled=true \
    bluetooth.profile.opp.enabled=true \
    bluetooth.profile.pan.nap.enabled=true \
    bluetooth.profile.pan.panu.enabled=true \
    bluetooth.profile.pbap.server.enabled=true \
    bluetooth.profile.sap.server.enabled=true \
    persist.bluetooth.a2dp_offload.disabled=true

# Wi-Fi: AIDL IWifi for interface management, wificond for nl80211 scans,
# and wpa_supplicant for authentication. QCA offloads remain optional with
# ath10k; hardware/qcom/wlan supplies the defaults wrapper for the legacy HAL.
PRODUCT_SOONG_NAMESPACES += hardware/qcom/wlan hardware/qcom/wlan/legacy
PRODUCT_PACKAGES += \
    android.hardware.wifi-service \
    hostapd \
    wpa_supplicant \
    wpa_supplicant.conf \
    libwpa_client
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    $(LOCAL_PATH)/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf

# Touch / USB host / misc features
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml

# Modem bring-up helpers (QRTR userspace, remote fs, TFTP, PD mapper)
PRODUCT_PACKAGES += \
    rmtfs \
    tqftpserv \
    pd-mapper \
    libqrtr \
    qrtr-lookup

# The modem partition supplies the matching remoteproc, WLAN and IPA firmware.
# Keep the mountpoint in the read-only vendor image for fs_mgr's early mount.
PRODUCT_PACKAGES += \
    joan_firmware_mountpoint \
    joan_remoteproc_firmware \
    joan_wlan_dsp_firmware \
    joan_wlan_board_data \
    joan_ipa_firmware_mdt \
    joan_ipa_firmware_b00 \
    joan_ipa_firmware_b01 \
    joan_ipa_firmware_b02 \
    joan_ipa_firmware_b03 \
    joan_ipa_firmware_b04

# Other extracted firmware and proprietary userspace libraries.
$(call inherit-product-if-exists, vendor/lge/joan/joan-vendor.mk)
$(call inherit-product-if-exists, vendor/lge/joan/radio/radio-vendor.mk)

# NFC (PN548 through /dev/pn54x; no firmware library is shipped, so the HAL
# never reflashes the controller). Configuration from the LG vendor image.
PRODUCT_PACKAGES += \
    android.hardware.nfc@1.2-service \
    com.android.nfc_extras \
    Tag
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/nfc/libnfc-nci.conf:$(TARGET_COPY_OUT_VENDOR)/etc/libnfc-nci.conf \
    $(LOCAL_PATH)/nfc/libnfc-nxp.conf:$(TARGET_COPY_OUT_VENDOR)/etc/libnfc-nxp.conf \
    frameworks/native/data/etc/android.hardware.nfc.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.xml \
    frameworks/native/data/etc/android.hardware.nfc.hce.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.hce.xml \
    frameworks/native/data/etc/android.hardware.nfc.hcef.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.hcef.xml \
    frameworks/native/data/etc/com.android.nfc_extras.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.android.nfc_extras.xml

# Camera: upstream libcamera (simple pipeline + software ISP on CAMSS) as a
# camera3 HAL module behind Lineage's AIDL provider; see prebuilts/libcamera.
PRODUCT_PACKAGES += android.hardware.camera.provider-service.lineage
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/prebuilts/libcamera/lib64/libcamera.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libcamera.so \
    $(LOCAL_PATH)/prebuilts/libcamera/lib64/libcamera-base.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libcamera-base.so \
    $(LOCAL_PATH)/prebuilts/libcamera/lib64/hw/camera.libcamera.so:$(TARGET_COPY_OUT_VENDOR)/lib64/hw/camera.libcamera.so \
    $(LOCAL_PATH)/prebuilts/libcamera/lib64/libcamera/ipa/ipa_softisp.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libcamera/ipa/ipa_softisp.so \
    $(LOCAL_PATH)/prebuilts/libcamera/lib64/libcamera/ipa/ipa_softisp.so.sign:$(TARGET_COPY_OUT_VENDOR)/lib64/libcamera/ipa/ipa_softisp.so.sign \
    $(LOCAL_PATH)/prebuilts/libcamera/bin/libcamera/softisp_ipa_proxy:$(TARGET_COPY_OUT_VENDOR)/bin/libcamera/softisp_ipa_proxy \
    $(LOCAL_PATH)/prebuilts/libcamera/etc/libcamera/ipa/softisp/hi553.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/ipa/softisp/hi553.yaml \
    $(LOCAL_PATH)/prebuilts/libcamera/etc/libcamera/ipa/softisp/imx351.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/ipa/softisp/imx351.yaml \
    $(LOCAL_PATH)/prebuilts/libcamera/etc/libcamera/ipa/softisp/s5k3m3.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/ipa/softisp/s5k3m3.yaml \
    $(LOCAL_PATH)/prebuilts/libcamera/etc/libcamera/ipa/softisp/uncalibrated.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/ipa/softisp/uncalibrated.yaml \
    $(LOCAL_PATH)/camera/camera_hal.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/camera_hal.yaml \
    $(LOCAL_PATH)/prebuilts/libcamera/etc/libcamera/configuration.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/configuration.yaml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml \
    $(LOCAL_PATH)/configs/permissions/android.hardware.camera.flash-joan.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.flash-joan.xml
PRODUCT_VENDOR_PROPERTIES += ro.hardware.camera=libcamera

# Power HAL: mainline DRM has no display idle_state node, so every touch
# held the INTERACTION boost (big >= 1344 MHz) for 1.4-5.65 s. Keep the
# boost short instead; touches during a scroll renew it.
PRODUCT_VENDOR_PROPERTIES += \
    vendor.powerhal.disp.idle_support=false \
    vendor.powerhal.interaction.min=500 \
    vendor.powerhal.interaction.max=2500 \
    vendor.powerhal.interaction.offset=200

# Telephony: Qualcomm qcrild + netmgrd from the 22.2 vendor image (see
# extract-radio.py); the AOSP pieces they link against are built here.
PRODUCT_PACKAGES += \
    android.hardware.radio@1.0.vendor \
    android.hardware.radio@1.1.vendor \
    android.hardware.radio@1.2.vendor \
    android.hardware.radio@1.3.vendor \
    android.hardware.radio@1.4.vendor \
    android.hardware.radio@1.5.vendor \
    android.hardware.radio.config@1.0.vendor \
    android.hardware.radio.config@1.1.vendor \
    android.hardware.radio.config@1.2.vendor \
    android.hardware.radio.deprecated@1.0.vendor \
    android.hardware.secure_element@1.0.vendor \
    android.hardware.secure_element@1.1.vendor \
    android.hardware.secure_element@1.2.vendor \
    libprotobuf-cpp-full-3.9.1-vendorcompat \
    android.system.net.netd@1.0.vendor \
    android.system.net.netd@1.1.vendor \
    libnetutils.vendor \
    libqti_vndfwk_detect.vendor \
    libsqlite.vendor
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.telephony.cdma.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.cdma.xml
PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.radio.apm_sim_not_pwdn=1 \
    persist.vendor.radio.custom_ecc=1 \
    persist.vendor.radio.mt_sms_ack=30 \
    persist.vendor.radio.procedure_bytes=SKIP \
    persist.vendor.radio.rat_on=combine \
    persist.vendor.radio.sib16_support=1 \
    persist.vendor.radio.add_power_save=1 \
    persist.vendor.data.iwlan.enable=false \
    persist.vendor.cne.feature=0 \
    persist.vendor.dpm.feature=0 \
    ril.subscription.types=NV,RUIM \
    ro.telephony.default_network=10 \
    telephony.lteOnCdmaDevice=1

# Power HAL profile (touch / launch boosts) for the mainline cpufreq and
# GPU devfreq nodes; adapted from the 22.2 joan profile
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/power/powerhint.json:$(TARGET_COPY_OUT_VENDOR)/etc/powerhint.json
PRODUCT_SOONG_NAMESPACES += \
    hardware/google/interfaces \
    hardware/google/pixel \
    hardware/google/pixel/power-libperfmgr \
    hardware/lineage/interfaces/power-libperfmgr

# Device Quick Settings: mainline Quad DAC and stepped torch control.
PRODUCT_PACKAGES += JoanDeviceParts
