#!/system/bin/sh
# Start firmware users only after the filesystem services are available.
# Remoteproc firmware and modem_pr are served from the phone's own modem partition.

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
	# Firmware/QMI initialization is asynchronous after modprobe returns.
	# Allow the modem's WLAN domain to publish the service and netdev.
	while [ "$n" -lt 180 ]; do
		[ -d /sys/class/net/wlan0 ] && return 0
		sleep 1
		n=$((n + 1))
	done
	return 1
}

# Do not fall back to firmware copied from another device variant.
if ! grep -q ' /vendor/firmware_mnt ' /proc/mounts; then
	echo "modem firmware partition is not mounted; leaving remote processors offline"
	exit 1
fi

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

# Modem firmware publishes WLFW asynchronously. Notify Android only after
# the netdev exists; failures do not crash or restart a remote processor.
publish_wlan() {
	wait_wlan || return 1
	setprop vendor.joan.wlan.ready 1
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
	# The SLIMbus NGD controller feeds the WCD934x codec and the sound card
	# cannot bind without it. Register audio before starting the WLAN client.
	load slim-qcom-ngd-ctrl || echo "SLIMbus/audio did not come up; see dmesg"

	# Register the WLAN client before starting the modem's WLAN domain.
	load ath10k_snoc || echo "WLAN driver did not load"
}

# Register both PAS instances first so pd-mapper can read every .jsn
# (adsp*/slpi* from qcom_q6v5_pas, modem* from qcom_q6v5_mss). Both
# processors stay offline: the ADSP has qcom,no-auto-boot and the modem is
# only started below.
load qcom_q6v5_pas || echo "PAS driver did not load"
load qcom_q6v5_mss || echo "MSS driver did not load"

# The userspace locator owns the SERVREG service and must be running before
# any processor boots and registers its protection domains.
setprop ctl.start pd-mapper
n=0
while [ "$(getprop init.svc.pd-mapper)" != running ] && [ "$n" -lt 20 ]; do
	sleep 0.5
	n=$((n + 1))
done
sleep 1

start_adsp_clients || echo "ADSP/audio did not finish initialization; see dmesg"
# Start the modem after ADSP/audio and the WLAN client are registered.
start_modem &
modem_pid=$!
# init kills the whole oneshot service cgroup when this shell exits. Reap the
# bounded modem startup even when the independent ADSP/WLAN branch fails.
wait "$modem_pid" || echo "modem did not reach running; see dmesg"
publish_wlan || echo "WLAN initialization failed; see dmesg"

echo "=== remoteproc bring-up complete at $(up)"
ip -br link show wlan0
for r in /sys/class/remoteproc/remoteproc*; do
	echo "$(cat "$r/name"): $(cat "$r/state")"
done
