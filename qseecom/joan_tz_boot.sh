#!/system/bin/sh
# Load TrustZone (QSEECOM) + UFS RPMB + FPC1020 fingerprint kernel modules
# early in post-fs so qseecomd and hardware gatekeeper are ready for early_hal.

MODDIR="/vendor/lib/modules"

for m in cma_heap.ko ion_legacy.ko sg.ko qseecom_joan.ko fpc1020_tee.ko; do
    if [ -f "${MODDIR}/${m}" ]; then
        insmod "${MODDIR}/${m}" 2>/dev/null || true
    fi
done

# Provide Qualcomm legacy /dev/block/bootdevice symlink for qseecomd (libssd)
if [ -d /dev/block/platform/soc@0/1da4000.ufshc ]; then
    ln -sfn /dev/block/platform/soc@0/1da4000.ufshc /dev/block/bootdevice
fi

# Set permissions for QSEECOM, UFS RPMB sg node, and FPC1020 sysfs interface
if [ -e /dev/qseecom ]; then
    chown system:drmrpc /dev/qseecom 2>/dev/null || true
    chmod 0666 /dev/qseecom 2>/dev/null || true
fi

for sg in /dev/sg*; do
    if [ -e "${sg}" ]; then
        chmod 0660 "${sg}" 2>/dev/null || true
    fi
done

FP_SYS="/sys/devices/virtual/input/lge_fingerprint"
if [ -d "${FP_SYS}" ]; then
    for attr in irq wakeup_enable hw_reset device_prepare regulator_enable pinctl_set clk_enable; do
        if [ -e "${FP_SYS}/${attr}" ]; then
            chown system:system "${FP_SYS}/${attr}" 2>/dev/null || true
            chmod 0660 "${FP_SYS}/${attr}" 2>/dev/null || true
        fi
    done
fi

exit 0
