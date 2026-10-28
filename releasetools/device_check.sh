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

# Preserve any failure so recovery never reports success with partial firmware.
cleanup() {
    result=$?
    trap - EXIT
    $T umount /mnt/vendor || result=1
    exit "$result"
}
trap cleanup EXIT
set -e

if $T grep -q "LG-H932" /proc/cmdline; then
    echo "H932: installing its Adreno zap shader"
    variant=H932
    other=H930
else
    echo "Not an H932: installing the H930 Adreno zap shader"
    variant=H930
    other=H932
fi

# The vendor image is packed tightly. Copying even a small file can hit ENOSPC
# (and truncate the existing shader). Free the unused variant first, then use
# same-filesystem renames instead of allocating duplicate data blocks.
for suffix in b00 b01 b02 elf mdt; do
    [ -s "$FW/$variant/a540_zap.$suffix" ]
done
$T rm -r "$FW/$other"
$T mv "$FW/$variant"/a540_zap.* "$FW/"
$T rmdir "$FW/$variant"
$T chmod 0644 $FW/a540_zap.*
$T chcon u:object_r:firmware_file:s0 $FW/a540_zap.*
exit 0
