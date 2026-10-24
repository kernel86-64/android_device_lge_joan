#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit some common Lineage stuff.
$(call inherit-product, vendor/lineage/config/common_full_phone.mk)

# Inherit from joan device
$(call inherit-product, device/lge/joan/device.mk)

# Device identifier. This must come after all inclusions
PRODUCT_DEVICE := joan
PRODUCT_NAME := lineage_joan
PRODUCT_BRAND := lge
PRODUCT_MODEL := LG V30
PRODUCT_MANUFACTURER := LGE

PRODUCT_GMS_CLIENTID_BASE := android-lge
PRODUCT_RELEASE_NAME := V30
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := 560dpi
