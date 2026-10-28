#!/usr/bin/env python3
"""Exercise actual startup control flow with firmware operations mocked out."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "rootdir/bin/joan_rprocs.sh").read_text()
functions = "\n".join(
    re.search(r"^" + name + r"\(\) \{\n.*?^\}", source, re.M | re.S).group()
    for name in ("start_wlan", "start_adsp_clients")
)
tail = source[source.index("\nstart_modem &") : source.index('\necho "=== remoteproc bring-up complete')]

for scenario, expect_audio in (
    ("success", True),
    ("wlan_module_failure", True),
    ("wlan_retry_failure", True),
    ("adsp_module_failure", False),
    ("adsp_offline_after_wlan", False),
):
    with tempfile.TemporaryDirectory(prefix="joan-rprocs-test-") as directory:
        # No firmware/sysfs writes or module operations can escape the fixture.
        body = functions.replace("/sys/", directory + "/sys/")
        harness = r'''
load() {
    echo "load $1" >> "$EVENTS"
    case "$SCENARIO:$1" in
        wlan_module_failure:ath10k_snoc|adsp_module_failure:qcom_q6v5_pas) return 1 ;;
    esac
    return 0
}
rproc_find() { echo /mock/adsp; }
wait_running() {
    [ "$SCENARIO:$2" != adsp_offline_after_wlan:60 ]
}
wait_wlan() { [ "$SCENARIO" != wlan_retry_failure ]; }
setprop() { echo "property $*" >> "$EVENTS"; }
up() { echo 0; }
start_modem() {
    sleep 0.15
    echo modem_done >> "$EVENTS"
}
'''
        result = subprocess.run(
            ["bash", "-c", harness + body + tail + '\necho parent_done >> "$EVENTS"\n'],
            env={**os.environ, "SCENARIO": scenario, "EVENTS": directory + "/events"},
            capture_output=True, text=True, timeout=5,
        )
        events = Path(directory, "events").read_text().splitlines()
        assert result.returncode == 0, result.stderr
        assert ("load slim-qcom-ngd-ctrl" in events) == expect_audio, (scenario, events)
        assert events.index("modem_done") < events.index("parent_done"), (scenario, events)
        print(f"PASS {scenario}: audio={expect_audio}, modem completed before parent exit")
