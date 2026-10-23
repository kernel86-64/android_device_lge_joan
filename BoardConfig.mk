#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# LG V30 (joan, MSM8998) on a mainline-based 6.18 kernel.
#

DEVICE_PATH := device/lge/joan

BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

# Architecture
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_VARIANT := cortex-a73

# Bootloader
TARGET_BOOTLOADER_BOARD_NAME := msm8998
TARGET_NO_BOOTLOADER := true
TARGET_OTA_ASSERT_DEVICE := v30,joan,h930,h930ds,h931,h932,h933,us998,vs996,ls998

# Kernel
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
BOARD_BOOT_HEADER_VERSION := 0
# LG's ABL loads the kernel at +0x8000; the ramdisk must sit past the
# ~40 MiB uncompressed kernel or ABL refuses the image, hence +0x04000000.
BOARD_MKBOOTIMG_ARGS := \
    --header_version $(BOARD_BOOT_HEADER_VERSION) \
    --kernel_offset 0x00008000 \
    --ramdisk_offset 0x04000000 \
    --tags_offset 0x00000100
BOARD_KERNEL_CMDLINE := \
    androidboot.hardware=joan \
    androidboot.boot_devices=soc@0/1da4000.ufshc \
    androidboot.super_partition=system \
    androidboot.selinux=permissive \
    firmware_class.path=/vendor/firmware \
    loop.max_part=7 \
    log_buf_len=4M
BOARD_KERNEL_IMAGE_NAME := Image.gz
BOARD_KERNEL_APPEND_DTBS := qcom/msm8998-lg-joan.dtb
TARGET_KERNEL_SOURCE := kernel/lge/joan
TARGET_KERNEL_CONFIG := joan_defconfig
TARGET_KERNEL_VERSION := 6.18
# Modules: everything the kernel build produces goes to /vendor/lib/modules;
# init.joan.rc loads the firmware-dependent ones after /vendor is up.
BOARD_VENDOR_KERNEL_MODULES_LOAD := \
    qcom_q6v5_mss qcom_q6v5_pas venus-core venus-dec venus-enc ipa \
    hci_uart ath10k_snoc

# Partitions
# A-only device (Android 16 defaults to A/B).
AB_OTA_UPDATER := false
BOARD_BOOTIMAGE_PARTITION_SIZE := 41943040
BOARD_CACHEIMAGE_PARTITION_SIZE := 536870912
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 41943040
BOARD_FLASH_BLOCK_SIZE := 0x40000
BOARD_HAS_LARGE_FILESYSTEM := true

# Dynamic partitions are retrofitted inside the LG system partition, exactly
# like the LineageOS 22.2 joan builds, so the same flashing flow applies.
BOARD_SUPER_PARTITION_METADATA_DEVICE := system
BOARD_SUPER_PARTITION_SIZE := 5863636992
BOARD_SUPER_PARTITION_GROUPS := qti_dynamic_partitions
BOARD_QTI_DYNAMIC_PARTITIONS_PARTITION_LIST := odm product system system_ext vendor
BOARD_QTI_DYNAMIC_PARTITIONS_SIZE := 5859442688
-include vendor/lineage/config/BoardConfigReservedSize.mk

TARGET_USERIMAGES_USE_EXT4 := true
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_ODMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4

TARGET_COPY_OUT_ODM := odm
TARGET_COPY_OUT_PRODUCT := product
TARGET_COPY_OUT_SYSTEM_EXT := system_ext
TARGET_COPY_OUT_VENDOR := vendor

# Graphics (Mesa freedreno + drm_hwcomposer + minigbm)
BOARD_USE_CUSTOMIZED_MESA := true
TARGET_USES_HWC2 := true

# Recovery
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/rootdir/etc/fstab.joan
TARGET_RECOVERY_PIXEL_FORMAT := RGBX_8888
TARGET_USERIMAGES_USE_F2FS := true

# Releasetools
TARGET_RELEASETOOLS_EXTENSIONS := $(DEVICE_PATH)/releasetools

# SELinux
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/vendor

# VINTF
DEVICE_MANIFEST_FILE := $(DEVICE_PATH)/manifest.xml

# Wi-Fi (ath10k-snoc, upstream nl80211)
WPA_SUPPLICANT_VERSION := VER_0_8_X
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_WLAN_DEVICE := wlan0

# Bluetooth (WCN3990 via hci_qca, Linux HCI user channel)
BOARD_HAVE_BLUETOOTH := true

# Proprietary firmware extracted from the phone
-include vendor/lge/joan/BoardConfigVendor.mk
