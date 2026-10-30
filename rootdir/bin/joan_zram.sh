#!/system/bin/sh
# Bring up zram swap if init's swapon_all did not. Without it the phone
# runs with no swap at all and memory-heavy apps get killed or thrash the
# page cache.
grep -q zram0 /proc/swaps && exit 0
Z=/sys/block/zram0
[ "$(cat $Z/disksize)" = 0 ] || echo 1 > $Z/reset
echo lz4 > $Z/comp_algorithm
echo 3221225472 > $Z/disksize
mkswap /dev/block/zram0 && swapon /dev/block/zram0
