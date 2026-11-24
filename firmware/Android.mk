LOCAL_PATH := $(call my-dir)

# fsgen's firmware path mapping cannot express this sibling mountpoint.
# Install its placeholder directly into the vendor partition.
include $(CLEAR_VARS)
LOCAL_MODULE := joan_firmware_mountpoint
LOCAL_MODULE_CLASS := ETC
LOCAL_VENDOR_MODULE := true
LOCAL_SRC_FILES := mountpoint.keep
LOCAL_MODULE_STEM := .keep
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/firmware_mnt
include $(BUILD_PREBUILT)
