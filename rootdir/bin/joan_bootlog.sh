#!/system/bin/sh
# Keep diagnostics on unencrypted /data so recovery can read a wedged boot.
# This must be a script: init expands '$' in service command arguments before
# invoking a shell, so inline shell variables/command substitutions are unsafe.
# Development builds only (androidboot.joan.bootlog=1): logcat holds user data.
[ "$(getprop ro.boot.joan.bootlog)" = 1 ] || exit 0
umask 022
duration=10800
sample_interval=10
D=/data/local/tmp/joan/$(cat /proc/sys/kernel/random/boot_id | cut -c1-8)
mkdir -p "$D/pstore" || exit 1
chmod 755 /data/local/tmp/joan "$D" "$D/pstore"
for f in /sys/fs/pstore/*; do
	[ -f "$f" ] && cat "$f" > "$D/pstore/${f##*/}"
done
ls -l /sys/fs/pstore > "$D/pstore-list.txt" 2>&1
cat /proc/cmdline > "$D/cmdline.txt"
getprop > "$D/properties.txt"

# Android init disables hung-task detection. Collect blocked task stacks
# during a debug capture without causing a reboot for a blocked task.
if [ -w /proc/sys/kernel/hung_task_timeout_secs ]; then
	echo 0 > /proc/sys/kernel/hung_task_panic
	echo 30 > /proc/sys/kernel/hung_task_timeout_secs
fi

timeout -k 2 "$duration" dmesg -w > "$D/dmesg.txt" 2>&1 &
kernel_log=$!
timeout -k 2 "$duration" logcat -b all -v threadtime -f "$D/logcat.txt" -r 8192 -n 4 &
android_log=$!
trap 'kill "$kernel_log" "$android_log" 2>/dev/null' EXIT

i=0
while [ "$i" -lt "$((duration / sample_interval))" ]; do
	{
		echo "=== $(cat /proc/uptime)"
		cat /proc/loadavg /proc/pressure/cpu /proc/pressure/memory /proc/pressure/io
		grep -E 'MemAvailable|SwapFree|Dirty|Writeback' /proc/meminfo
		printf 'suspend success=%s fail=%s\n' \
			"$(cat /sys/power/suspend_stats/success)" \
			"$(cat /sys/power/suspend_stats/fail)"
		grep -E '^POWER_SUPPLY_(STATUS|VOLTAGE_NOW|CURRENT_NOW|CAPACITY|TEMP)=' \
			/sys/class/power_supply/qcom-battery/uevent
		for r in /sys/class/remoteproc/remoteproc*; do
			echo "$(cat "$r/name"): $(cat "$r/state")"
		done
	} >> "$D/heartbeat.txt" 2>&1
	# Android's toybox sync flushes globally and has no per-file mode. Keep
	# periodic crash evidence without forcing every filesystem out each tick.
	if [ $((i % 6)) -eq 0 ]; then
		sync
	fi
	sleep "$sample_interval"
	i=$((i + 1))
done
