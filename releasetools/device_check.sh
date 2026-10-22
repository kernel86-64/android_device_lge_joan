#!/sbin/sh
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# The H932 (T-Mobile) has its own signed Adreno 540 zap shader; every other
# V30 uses the H930 one. Install the matching set as qcom/a540_zap.*.

FW=/mnt/vendor/firmware/qcom

if [ -x /sbin/toybox ]; then T=/sbin/toybox; else T=/tmp/toybox; fi
$T mkdir -p /mnt/vendor
$T mount /dev/block/mapper/vendor -t ext4 /mnt/vendor || exit 1

if $T grep -q "LG-H932" /proc/cmdline; then
    echo "H932: installing its Adreno zap shader"
    $T cp $FW/H932/a540_zap.* $FW/
else
    echo "Not an H932: installing the H930 Adreno zap shader"
    $T cp $FW/H930/a540_zap.* $FW/
fi
$T rm -r $FW/H930 $FW/H932
$T chmod 0644 $FW/a540_zap.*
$T chcon u:object_r:firmware_file:s0 $FW/a540_zap.*

$T umount /mnt/vendor
exit 0
