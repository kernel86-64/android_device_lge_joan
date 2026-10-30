#!/system/bin/sh
# Keep diagnostics on unencrypted /data so recovery can read a wedged boot.
# This must be a script: init expands '$' in service command arguments before
# invoking a shell, so inline shell variables/command substitutions are unsafe.
# Development builds only (androidboot.joan.bootlog=1): logcat holds user data.
[ "$(getprop ro.boot.joan.bootlog)" = 1 ] || exit 0
umask 022
D=/data/local/tmp/joan/$(cat /proc/sys/kernel/random/boot_id | cut -c1-8)
mkdir -p "$D/pstore" || exit 1
chmod 755 /data/local/tmp/joan "$D" "$D/pstore"
for f in /sys/fs/pstore/*; do
	[ -f "$f" ] && cat "$f" > "$D/pstore/${f##*/}"
done
ls -l /sys/fs/pstore > "$D/pstore-list.txt" 2>&1
cat /proc/cmdline > "$D/cmdline.txt"
getprop > "$D/properties.txt"
timeout -k 2 3600 dmesg -w > "$D/dmesg.txt" 2>&1 &
kernel_log=$!
timeout -k 2 3600 logcat -b all -v threadtime -f "$D/logcat.txt" -r 8192 -n 4 &
android_log=$!
trap 'kill "$kernel_log" "$android_log" 2>/dev/null' EXIT

i=0
while [ "$i" -lt 360 ]; do
	{
		echo "=== $(cat /proc/uptime)"
		cat /proc/loadavg /proc/pressure/cpu /proc/pressure/memory /proc/pressure/io
		grep -E 'MemAvailable|SwapFree|Dirty|Writeback' /proc/meminfo
		for r in /sys/class/remoteproc/remoteproc*; do
			echo "$(cat "$r/name"): $(cat "$r/state")"
		done
	} >> "$D/heartbeat.txt" 2>&1
	# Android's toybox sync flushes globally and has no per-file mode. Keep
	# periodic crash evidence without forcing every filesystem out each tick.
	if [ $((i % 6)) -eq 0 ]; then
		sync
	fi
	sleep 10
	i=$((i + 1))
done
