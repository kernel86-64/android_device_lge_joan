/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Vibrator HAL for the LG V30 (joan): the DW7800 LRA driver is a memless
 * force-feedback input device (FF_RUMBLE), so a vibration is one uploaded
 * rumble effect played for the requested duration.
 */

#define LOG_TAG "vibrator.joan"

#include <aidl/android/hardware/vibrator/BnVibrator.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace aidl::android::hardware::vibrator {

using ::android::base::unique_fd;

static constexpr char kDeviceName[] = "dw7800-haptics";

#define UNSUPPORTED \
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION)

class Vibrator : public BnVibrator {
  public:
    Vibrator() { mFd = openDevice(); }

    ndk::ScopedAStatus getCapabilities(int32_t* caps) override {
        *caps = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                IVibrator::CAP_AMPLITUDE_CONTROL;
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus off() override {
        std::lock_guard<std::mutex> lock(mMutex);
        stopLocked();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus on(int32_t timeoutMs,
                          const std::shared_ptr<IVibratorCallback>& callback) override {
        return vibrate(timeoutMs, callback);
    }

    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* durationMs) override {
        int32_t ms;
        float amplitude;

        switch (effect) {
            case Effect::TICK:
            case Effect::TEXTURE_TICK:
                ms = 10;
                break;
            case Effect::CLICK:
                ms = 20;
                break;
            case Effect::HEAVY_CLICK:
                ms = 30;
                break;
            case Effect::DOUBLE_CLICK:
                ms = 60;
                break;
            default:
                UNSUPPORTED;
        }
        switch (strength) {
            case EffectStrength::LIGHT:
                amplitude = 0.5f;
                break;
            case EffectStrength::MEDIUM:
                amplitude = 0.75f;
                break;
            default:
                amplitude = 1.0f;
                break;
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mAmplitude = amplitude;
        }
        *durationMs = ms;
        return vibrate(ms, callback);
    }

    ndk::ScopedAStatus performVendorEffect(const VendorEffect&,
                                           const std::shared_ptr<IVibratorCallback>&) override {
        UNSUPPORTED;
    }

    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* effects) override {
        *effects = {Effect::TICK, Effect::TEXTURE_TICK, Effect::CLICK, Effect::HEAVY_CLICK,
                    Effect::DOUBLE_CLICK};
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus setAmplitude(float amplitude) override {
        if (amplitude <= 0.0f || amplitude > 1.0f)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        std::lock_guard<std::mutex> lock(mMutex);
        mAmplitude = amplitude;
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus setExternalControl(bool) override { UNSUPPORTED; }
    ndk::ScopedAStatus getCompositionDelayMax(int32_t*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getCompositionSizeMax(int32_t*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getSupportedPrimitives(std::vector<CompositePrimitive>* p) override {
        p->clear();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive, int32_t*) override {
        UNSUPPORTED;
    }
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>&,
                               const std::shared_ptr<IVibratorCallback>&) override {
        UNSUPPORTED;
    }
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* e) override {
        e->clear();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus alwaysOnEnable(int32_t, Effect, EffectStrength) override { UNSUPPORTED; }
    ndk::ScopedAStatus alwaysOnDisable(int32_t) override { UNSUPPORTED; }
    ndk::ScopedAStatus getResonantFrequency(float*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getQFactor(float*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getFrequencyResolution(float*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getFrequencyMinimum(float*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>*) override { UNSUPPORTED; }
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>&,
                                   const std::shared_ptr<IVibratorCallback>&) override {
        UNSUPPORTED;
    }
    ndk::ScopedAStatus getFrequencyToOutputAccelerationMap(
            std::vector<FrequencyAccelerationMapEntry>*) override {
        UNSUPPORTED;
    }
    ndk::ScopedAStatus getPwleV2PrimitiveDurationMaxMillis(int32_t*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getPwleV2PrimitiveDurationMinMillis(int32_t*) override { UNSUPPORTED; }
    ndk::ScopedAStatus getPwleV2CompositionSizeMax(int32_t*) override { UNSUPPORTED; }
    ndk::ScopedAStatus composePwleV2(const CompositePwleV2&,
                                     const std::shared_ptr<IVibratorCallback>&) override {
        UNSUPPORTED;
    }

  private:
    static unique_fd openDevice() {
        std::unique_ptr<DIR, decltype(&closedir)> dir(opendir("/dev/input"), closedir);

        if (!dir)
            return unique_fd();
        while (struct dirent* ent = readdir(dir.get())) {
            char name[64] = {};
            std::string path = std::string("/dev/input/") + ent->d_name;

            if (strncmp(ent->d_name, "event", 5))
                continue;
            unique_fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC));
            if (fd < 0)
                continue;
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && !strcmp(name, kDeviceName)) {
                LOG(INFO) << "using " << path;
                return fd;
            }
        }
        LOG(ERROR) << "no " << kDeviceName << " input device";
        return unique_fd();
    }

    void stopLocked() {
        if (mFd < 0 || mEffectId < 0)
            return;
        struct input_event ev = {};
        ev.type = EV_FF;
        ev.code = mEffectId;
        ev.value = 0;
        (void)write(mFd, &ev, sizeof(ev));
        mGeneration++;
    }

    ndk::ScopedAStatus vibrate(int32_t ms, const std::shared_ptr<IVibratorCallback>& callback) {
        std::lock_guard<std::mutex> lock(mMutex);

        if (mFd < 0)
            mFd = openDevice();
        if (mFd < 0)
            return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
        if (ms <= 0)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);

        stopLocked();

        struct ff_effect effect = {};
        effect.type = FF_RUMBLE;
        effect.id = mEffectId;
        effect.u.rumble.strong_magnitude = static_cast<uint16_t>(mAmplitude * 0xffff);
        effect.replay.length = static_cast<uint16_t>(std::min(ms, 0xffff));
        if (ioctl(mFd, EVIOCSFF, &effect) < 0) {
            PLOG(ERROR) << "EVIOCSFF";
            return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
        }
        mEffectId = effect.id;

        struct input_event ev = {};
        ev.type = EV_FF;
        ev.code = mEffectId;
        ev.value = 1;
        if (write(mFd, &ev, sizeof(ev)) != sizeof(ev)) {
            PLOG(ERROR) << "play";
            return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
        }

        if (callback) {
            uint64_t generation = ++mGeneration;
            std::thread([this, ms, callback, generation] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (generation != mGeneration)
                        return;
                }
                callback->onComplete();
            }).detach();
        }
        return ndk::ScopedAStatus::ok();
    }

    std::mutex mMutex;
    unique_fd mFd;
    int16_t mEffectId = -1;
    float mAmplitude = 1.0f;
    uint64_t mGeneration = 0;
};

}  // namespace aidl::android::hardware::vibrator

int main() {
    using aidl::android::hardware::vibrator::Vibrator;

    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto vib = ndk::SharedRefBase::make<Vibrator>();
    const std::string instance = std::string(Vibrator::descriptor) + "/default";
    CHECK_EQ(AServiceManager_addService(vib->asBinder().get(), instance.c_str()), STATUS_OK);
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
