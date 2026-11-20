#!/system/bin/sh
# Start firmware users only after the filesystem services are available.
# A late manual ADSP restart plus WLAN reprobe has produced working scans;
# this boot sequence still needs cold-boot validation on joan.

# Development builds (androidboot.joan.bootlog=1) trace this script.
if [ "$(getprop ro.boot.joan.bootlog)" = 1 ]; then
	D=/data/local/tmp/joan/$(cat /proc/sys/kernel/random/boot_id | cut -c1-8)
	mkdir -p "$D"
	chmod 755 /data/local/tmp/joan "$D"
	exec >>"$D/rprocs.log" 2>&1
	chmod 644 "$D/rprocs.log"
	set -x
else
	exec >/dev/null 2>&1
fi

up() { cut -d' ' -f1 /proc/uptime; }
load() { timeout -k 2 30 /vendor/bin/modprobe -d /vendor/lib/modules "$1"; }
rproc_find() {
	for r in /sys/class/remoteproc/remoteproc*; do
		[ "$(cat "$r/name" 2>/dev/null)" = "$1" ] || continue
		echo "$r"
		return 0
	done
	return 1
}
wait_running() {
	n=0
	while [ "$n" -lt "$2" ]; do
		[ "$(cat "$1/state" 2>/dev/null)" = running ] && return 0
		sleep 1
		n=$((n + 1))
	done
	return 1
}
wait_wlan() {
	n=0
	# Firmware/QMI initialization is asynchronous after modprobe returns. On
	# this board the first netdev appeared over 60 seconds later; unbinding at
	# 30 seconds interrupted a probe that was still making progress.
	while [ "$n" -lt 180 ]; do
		[ -d /sys/class/net/wlan0 ] && return 0
		sleep 1
		n=$((n + 1))
	done
	return 1
}

echo "=== remoteproc bring-up at $(up)"
n=0
while [ "$(getprop sys.boot_completed)" != 1 ] && [ "$n" -lt 240 ]; do
	sleep 1
	n=$((n + 1))
done
sleep 10
if [ "$(getprop init.svc.rmtfs)" != running ] ||
   [ "$(getprop init.svc.tqftpserv)" != running ]; then
	echo "filesystem services unavailable; leaving remote processors offline"
	exit 1
fi

# Modem startup is independent of ADSP/WLAN. A missing SIM, failed modem or
# slow EFS response must not prevent the other radios and audio from probing.
start_modem() {
	load qcom_q6v5_mss || return 1
	MSS=$(rproc_find 4080000.remoteproc) || return 1
	if [ "$(cat "$MSS/state")" = offline ]; then
		timeout -k 2 90 /system/bin/sh -c 'echo start > "$1/state"' sh "$MSS" || return 1
	fi
	wait_running "$MSS" 90
}

# WLAN recovery can fail without making the running ADSP unusable for audio.
# Keep every WLAN failure local so it cannot skip audio or the modem wait.
start_wlan() {
	load ath10k_snoc || return 1
	try=1
	while ! wait_wlan; do
		[ "$try" -lt 3 ] || return 1
		try=$((try + 1))
		echo "=== WLAN retry $try at $(up)"
		# Quiesce the old WLAN client before restarting its remote service.
		if [ -L /sys/bus/platform/devices/18800000.wifi/driver ]; then
			timeout -k 2 30 /system/bin/sh -c \
				'echo 18800000.wifi > /sys/bus/platform/drivers/ath10k_snoc/unbind' || return 1
		fi
		# crash is a debugfs file, not a sysfs remoteproc attribute. Resolve the
		# number from the ADSP name because probe order changes the numbering.
		CRASH=/sys/kernel/debug/remoteproc/${ADSP##*/}/crash
		[ -w "$CRASH" ] || return 1
		echo 1 > "$CRASH" || return 1
		# The restart is asynchronous. Give it time to leave running, but a
		# short recovery can finish between polls; netdev readiness below
		# decides whether reprobe worked in that case.
		n=0
		while [ "$(cat "$ADSP/state")" = running ] && [ "$n" -lt 100 ]; do
			sleep 0.1
			n=$((n + 1))
		done
		if [ "$n" -eq 100 ]; then
			echo "ADSP transition was not observed; checking readiness after WLAN reprobe"
		fi
		wait_running "$ADSP" 60 || return 1
		timeout -k 2 30 /system/bin/sh -c \
			'echo 18800000.wifi > /sys/bus/platform/drivers/ath10k_snoc/bind' || return 1
	done
	# Publish readiness once; do not repeatedly restart the HAL during recovery.
	setprop vendor.joan.wlan.ready 1
	# The framework can exhaust its early HAL recovery attempts before this
	# asynchronous netdev appears. After init refreshes the HAL, reapply only
	# an enabled saved setting; never turn Wi-Fi on when the user left it off.
	sleep 2
	case "$(settings get global wifi_on)" in
		1|2) timeout -k 2 15 cmd wifi set-wifi-enabled enabled ||
			echo "Wi-Fi saved-state handoff failed; see framework logs" ;;
	esac
}

start_adsp_clients() {
	load qcom_q6v5_pas || return 1
	ADSP=$(rproc_find adsp) || return 1
	# The ADSP does not auto-boot (the SLPI shares its driver and loads at
	# boot); start it here, once the filesystem services are ready.
	if [ "$(cat "$ADSP/state")" = offline ]; then
		timeout -k 2 30 /system/bin/sh -c 'echo start > "$1/state"' sh "$ADSP" || return 1
	fi
	wait_running "$ADSP" 30 || return 1
	# Audio first: the SLIMbus NGD controller feeds the WCD934x codec and
	# the sound card cannot bind without it. Do this before any WLAN
	# recovery, which deliberately crashes and restarts the ADSP.
	load slim-qcom-ngd-ctrl || echo "SLIMbus/audio did not come up; see dmesg"

	# WLAN recovery is opt-in while the 24.0 Wi-Fi bring-up is broken: its
	# ADSP crash/restart cycle takes minutes and disrupts audio and the modem.
	if [ "$(getprop persist.vendor.joan.wlan_recovery)" = 1 ]; then
		start_wlan || echo "WLAN initialization failed"
		wait_running "$ADSP" 60 || return 1
	else
		load ath10k_snoc || echo "WLAN driver did not load"
	fi
}

start_modem &
modem_pid=$!
start_adsp_clients || echo "ADSP/audio did not finish initialization; see dmesg"
# init kills the whole oneshot service cgroup when this shell exits. Reap the
# bounded modem startup even when the independent ADSP/WLAN branch fails.
wait "$modem_pid" || echo "modem did not reach running; see dmesg"

echo "=== remoteproc bring-up complete at $(up)"
ip -br link show wlan0
for r in /sys/class/remoteproc/remoteproc*; do
	echo "$(cat "$r/name"): $(cat "$r/state")"
done
