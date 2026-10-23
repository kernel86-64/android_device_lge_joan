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

# Bring-up debugging: trust the build host's adb key, adb on from boot
ifneq ($(wildcard $(LOCAL_PATH)/adb_keys),)
PRODUCT_ADB_KEYS := $(LOCAL_PATH)/adb_keys
# adb only (no MTP) while bringing up: MTP's function is served from
# system_server, and the gadget cannot start until every function is ready.
PRODUCT_SYSTEM_DEFAULT_PROPERTIES += persist.sys.usb.config=adb
endif

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
    $(LOCAL_PATH)/rootdir/etc/ueventd.joan.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc \
    $(LOCAL_PATH)/rootdir/etc/init.recovery.joan.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.joan.rc

# Releasetools (H932 zap shader selection)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/releasetools/device_check.sh:install/bin/device_check.sh

# Recovery: LG keeps the bootloader message 128 bytes into misc.
$(call soong_config_set,lineage_recovery,bootloader_message_offset,128)

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

# No Vulkan on Adreno 5xx with Mesa (turnip is a6xx+).
PRODUCT_VENDOR_PROPERTIES += ro.hardware.vulkan=none

# Health (battery from the PMI8998 fuel gauge via power_supply)
PRODUCT_PACKAGES += \
    android.hardware.health-service.example \
    android.hardware.health-service.example_recovery

# USB
PRODUCT_PACKAGES += \
    android.hardware.usb-service.example \
    android.hardware.usb.gadget-service.example

# Keymint / Gatekeeper (software for now; no QSEE on mainline)
PRODUCT_PACKAGES += \
    android.hardware.security.keymint-service \
    com.android.hardware.gatekeeper.nonsecure

# Power / Thermal / Vibrator
PRODUCT_PACKAGES += \
    android.hardware.power-service.example \
    com.android.hardware.thermal \
    android.hardware.vibrator-service.example

# Lights (panel backlight)
PRODUCT_PACKAGES += android.hardware.light-service.lineage

# Audio: AOSP AIDL audio + effects HALs (built-in default configuration for
# now; audioserver aborts without any audio HAL)
PRODUCT_PACKAGES += com.android.hardware.audio
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.audio.output.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.audio.output.xml

# Bluetooth (Linux HCI)
PRODUCT_PACKAGES += android.hardware.bluetooth-service.default
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml

# Wi-Fi (ath10k)
PRODUCT_PACKAGES += \
    android.hardware.wifi-service \
    hostapd \
    wpa_supplicant \
    libwpa_client
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.direct.xml

# Touch / USB host / misc features
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml

# Modem bring-up helpers (QRTR userspace, remote fs, TFTP)
PRODUCT_PACKAGES += \
    rmtfs \
    tqftpserv \
    libqrtr \
    qrtr-lookup

# Firmware extracted from the phone (see extract-firmware.sh)
$(call inherit-product-if-exists, vendor/lge/joan/joan-vendor.mk)
