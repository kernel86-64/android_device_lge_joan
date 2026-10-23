/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keep the wall clock across reboots on joan.
 *
 * The PM8998 RTC counts seconds but cannot be set from Linux (LG's boot
 * chain keeps it read-only), so it boots at 1970 plus uptime-since-reset.
 * Like Qualcomm's time_daemon, keep the difference between the wall clock
 * and the RTC in /data and apply it at boot; whenever the clock is set
 * (network time, the user), store the new difference.
 */

#define LOG_TAG "joan_timekeep"

#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#define RTC_DEV "/dev/rtc0"
#define OFFSET_FILE "/data/vendor/time/rtc_offset"
/* Anything before 2020 is an unset clock. */
#define SANE_EPOCH 1577836800LL

static int read_rtc(int64_t *secs)
{
    struct rtc_time tm;
    struct tm t = {};
    int fd = open(RTC_DEV, O_RDONLY | O_CLOEXEC);
    int ret;

    if (fd < 0)
        return -errno;
    ret = ioctl(fd, RTC_RD_TIME, &tm);
    close(fd);
    if (ret < 0)
        return -errno;

    t.tm_sec = tm.tm_sec;
    t.tm_min = tm.tm_min;
    t.tm_hour = tm.tm_hour;
    t.tm_mday = tm.tm_mday;
    t.tm_mon = tm.tm_mon;
    t.tm_year = tm.tm_year;
    *secs = timegm(&t);
    return 0;
}

static int load_offset(int64_t *offset)
{
    FILE *f = fopen(OFFSET_FILE, "r");
    long long v;
    int ok;

    if (!f)
        return -errno;
    ok = fscanf(f, "%lld", &v) == 1;
    fclose(f);
    if (!ok)
        return -EINVAL;
    *offset = v;
    return 0;
}

static void save_offset(int64_t offset)
{
    char tmp[] = OFFSET_FILE ".tmp";
    FILE *f = fopen(tmp, "w");

    if (!f) {
        ALOGE("cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "%lld\n", (long long)offset);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, OFFSET_FILE) < 0)
        ALOGE("cannot rename %s: %s", tmp, strerror(errno));
}

/* Arm a far-away absolute timer that is cancelled when the clock is set. */
static int arm_clock_watch(int tfd)
{
    struct itimerspec its = {};

    its.it_value.tv_sec = INT32_MAX;
    return timerfd_settime(tfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL);
}

int main(void)
{
    int64_t rtc, offset;
    struct timespec now;
    int tfd;

    if (read_rtc(&rtc) < 0) {
        ALOGE("cannot read %s", RTC_DEV);
        return 1;
    }

    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec < SANE_EPOCH && load_offset(&offset) == 0) {
        struct timeval tv = { .tv_sec = rtc + offset };

        if (settimeofday(&tv, NULL) < 0)
            ALOGE("settimeofday: %s", strerror(errno));
        else
            ALOGI("restored the wall clock (rtc %lld + offset %lld)", (long long)rtc,
                  (long long)offset);
    }

    tfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
    if (tfd < 0 || arm_clock_watch(tfd) < 0) {
        ALOGE("timerfd: %s", strerror(errno));
        return 1;
    }

    for (;;) {
        uint64_t expirations;

        if (read(tfd, &expirations, sizeof(expirations)) < 0 && errno != ECANCELED) {
            if (errno == EINTR)
                continue;
            ALOGE("timerfd read: %s", strerror(errno));
            return 1;
        }
        arm_clock_watch(tfd);

        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec < SANE_EPOCH || read_rtc(&rtc) < 0)
            continue;
        offset = now.tv_sec - rtc;
        save_offset(offset);
        ALOGI("clock set; stored rtc offset %lld", (long long)offset);
    }
}
