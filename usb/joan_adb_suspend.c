// SPDX-FileCopyrightText: The LineageOS Project
// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "joan_adb_suspend"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <hardware_legacy/power.h>
#include <log/log.h>

#define UDC_STATE "/sys/devices/platform/soc@0/a800000.usb/udc/a800000.usb/state"
#define WAKE_LOCK "joan-adb-usb"

int main(void)
{
    struct pollfd udc = {
        .fd = open(UDC_STATE, O_RDONLY | O_CLOEXEC),
        .events = POLLPRI,
    };
    bool held = false;

    if (udc.fd < 0) {
        ALOGE("Cannot open USB gadget state: %s", strerror(errno));
        return 1;
    }

    /*
     * Only run while the ADB FunctionFS service is ready. A host must
     * configure the gadget before we block suspend, and sysfs notifies this
     * fd when the USB state changes. SystemSuspend owns the wake lock for
     * this process and releases it if init stops us.
     */
    for (;;) {
        char state[32];
        ssize_t count = TEMP_FAILURE_RETRY(pread(udc.fd, state, sizeof(state) - 1, 0));
        if (count <= 0) {
            ALOGE("Cannot read USB gadget state: %s",
                  count < 0 ? strerror(errno) : "end of file");
            break;
        }
        state[count] = '\0';

        bool configured = strcmp(state, "configured\n") == 0;
        if (configured && !held) {
            if (acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK) != 0) {
                ALOGE("Cannot acquire USB wake lock");
                break;
            }
            held = true;
        } else if (!configured && held) {
            release_wake_lock(WAKE_LOCK);
            held = false;
        }

        if (TEMP_FAILURE_RETRY(poll(&udc, 1, -1)) < 0 ||
            (udc.revents & (POLLHUP | POLLNVAL))) {
            ALOGE("Lost the USB gadget state");
            break;
        }
    }

    if (held)
        release_wake_lock(WAKE_LOCK);
    close(udc.fd);
    return 1;
}
