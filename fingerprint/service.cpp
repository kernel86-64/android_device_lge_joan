// SPDX-License-Identifier: Apache-2.0
//
// Hardware AIDL IFingerprint (V5) service wrapping LG/Qualcomm's TrustZone
// FPC1020 fingerprint.msm8998.so for LG V30 (joan).

#define LOG_TAG "android.hardware.biometrics.fingerprint-service.joan"

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <endian.h>
#include <errno.h>
#include <hardware/fingerprint.h>
#include <hardware/hardware.h>
#include <hardware/hw_auth_token.h>
#include <log/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aidl::android::hardware::biometrics::fingerprint {

namespace {

constexpr const char* kFingerprintHalPath =
        "/vendor/lib64/hw/fingerprint.msm8998.so";
constexpr int32_t kSensorId = 0;
constexpr int32_t kMaxEnrollmentsPerUser = 5;
constexpr uint32_t kEnrollTimeoutSec = 60;
constexpr int32_t kTimedLockoutThreshold = 5;
constexpr int32_t kPermanentLockoutThreshold = 20;
constexpr int64_t kTimedLockoutDurationMs = 30000;

int64_t now_monotonic_ms() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

void wait_for_qseecomd() {
    char val[PROPERTY_VALUE_MAX] = {};
    for (int i = 0; i < 150; ++i) {
        property_get("vendor.sys.listeners.registered", val, "");
        if (strcmp(val, "true") == 0) {
            return;
        }
        usleep(100000);
    }
    ALOGW("Timed out waiting for vendor.sys.listeners.registered=true");
}

void to_legacy_hat(
        const ::aidl::android::hardware::keymaster::HardwareAuthToken& in,
        hw_auth_token_t* out) {
    memset(out, 0, sizeof(*out));
    out->version = HW_AUTH_TOKEN_VERSION;
    out->challenge = static_cast<uint64_t>(in.challenge);
    out->user_id = static_cast<uint64_t>(in.userId);
    out->authenticator_id = static_cast<uint64_t>(in.authenticatorId);
    out->authenticator_type =
            htobe32(static_cast<uint32_t>(in.authenticatorType));
    out->timestamp =
            htobe64(static_cast<uint64_t>(in.timestamp.milliSeconds));
    size_t mac_len = in.mac.size();
    if (mac_len > sizeof(out->hmac)) {
        mac_len = sizeof(out->hmac);
    }
    if (mac_len > 0) {
        memcpy(out->hmac, in.mac.data(), mac_len);
    }
}

void to_aidl_hat(
        const hw_auth_token_t& in,
        ::aidl::android::hardware::keymaster::HardwareAuthToken* out) {
    out->challenge = static_cast<int64_t>(in.challenge);
    out->userId = static_cast<int64_t>(in.user_id);
    out->authenticatorId = static_cast<int64_t>(in.authenticator_id);
    out->authenticatorType = static_cast<
            ::aidl::android::hardware::keymaster::HardwareAuthenticatorType>(
            be32toh(in.authenticator_type));
    out->timestamp.milliSeconds =
            static_cast<int64_t>(be64toh(in.timestamp));
    out->mac.assign(in.hmac, in.hmac + sizeof(in.hmac));
}

std::pair<AcquiredInfo, int32_t> map_acquired(fingerprint_acquired_info_t info) {
    if (info >= FINGERPRINT_ACQUIRED_VENDOR_BASE) {
        return {AcquiredInfo::VENDOR,
                static_cast<int32_t>(info - FINGERPRINT_ACQUIRED_VENDOR_BASE)};
    }
    switch (info) {
        case FINGERPRINT_ACQUIRED_GOOD:
            return {AcquiredInfo::GOOD, 0};
        case FINGERPRINT_ACQUIRED_PARTIAL:
            return {AcquiredInfo::PARTIAL, 0};
        case FINGERPRINT_ACQUIRED_INSUFFICIENT:
            return {AcquiredInfo::INSUFFICIENT, 0};
        case FINGERPRINT_ACQUIRED_IMAGER_DIRTY:
            return {AcquiredInfo::SENSOR_DIRTY, 0};
        case FINGERPRINT_ACQUIRED_TOO_SLOW:
            return {AcquiredInfo::TOO_SLOW, 0};
        case FINGERPRINT_ACQUIRED_TOO_FAST:
            return {AcquiredInfo::TOO_FAST, 0};
        case FINGERPRINT_ACQUIRED_DETECTED:
            return {AcquiredInfo::START, 0};
        default:
            return {AcquiredInfo::UNKNOWN, 0};
    }
}

std::pair<Error, int32_t> map_error(fingerprint_error_t err) {
    if (err >= FINGERPRINT_ERROR_VENDOR_BASE) {
        return {Error::VENDOR,
                static_cast<int32_t>(err - FINGERPRINT_ERROR_VENDOR_BASE)};
    }
    switch (err) {
        case FINGERPRINT_ERROR_HW_UNAVAILABLE:
            return {Error::HW_UNAVAILABLE, 0};
        case FINGERPRINT_ERROR_UNABLE_TO_PROCESS:
            return {Error::UNABLE_TO_PROCESS, 0};
        case FINGERPRINT_ERROR_TIMEOUT:
            return {Error::TIMEOUT, 0};
        case FINGERPRINT_ERROR_NO_SPACE:
            return {Error::NO_SPACE, 0};
        case FINGERPRINT_ERROR_CANCELED:
            return {Error::CANCELED, 0};
        case FINGERPRINT_ERROR_UNABLE_TO_REMOVE:
            return {Error::UNABLE_TO_REMOVE, 0};
        default:
            return {Error::UNKNOWN, 0};
    }
}

class JoanSession;

std::mutex g_active_session_lock;
std::weak_ptr<JoanSession> g_active_session;

class JoanCancellationSignal : public common::BnCancellationSignal {
 public:
    explicit JoanCancellationSignal(fingerprint_device_t* dev) : dev_(dev) {}

    ::ndk::ScopedAStatus cancel() override {
        if (dev_ && dev_->cancel) {
            int rc = dev_->cancel(dev_);
            if (rc != 0) {
                ALOGW("legacy cancel() returned %d", rc);
            }
        }
        return ::ndk::ScopedAStatus::ok();
    }

 private:
    fingerprint_device_t* dev_;
};

class JoanSession : public BnSession {
 public:
    JoanSession(int32_t sensor_id, int32_t user_id,
                std::shared_ptr<ISessionCallback> cb,
                fingerprint_device_t* dev)
        : sensor_id_(sensor_id),
          user_id_(user_id),
          cb_(std::move(cb)),
          dev_(dev) {
        std::string store =
                "/data/system/users/" + std::to_string(user_id_) + "/fpdata";
        mkdir(store.c_str(), 0700);
        if (dev_ && dev_->set_active_group) {
            int rc = dev_->set_active_group(
                    dev_, static_cast<uint32_t>(user_id_), store.c_str());
            if (rc != 0) {
                ALOGE("set_active_group(%d, %s) failed: %d", user_id_,
                      store.c_str(), rc);
            } else {
                ALOGI("set_active_group(%d, %s) ok", user_id_, store.c_str());
            }
        }
    }

    ~JoanSession() override = default;

    bool isClosed() const {
        std::lock_guard<std::mutex> lk(lock_);
        return closed_;
    }

    void handleNotify(const fingerprint_msg_t* msg) {
        if (!msg) {
            return;
        }
        std::shared_ptr<ISessionCallback> cb;
        {
            std::lock_guard<std::mutex> lk(lock_);
            if (closed_) {
                return;
            }
            cb = cb_;
        }
        if (!cb) {
            return;
        }

        switch (msg->type) {
            case FINGERPRINT_ERROR: {
                if (msg->data.error == FINGERPRINT_ERROR_LOCKOUT) {
                    {
                        std::lock_guard<std::mutex> lk(lock_);
                        lockout_until_ms_ =
                                now_monotonic_ms() + kTimedLockoutDurationMs;
                    }
                    cb->onLockoutTimed(kTimedLockoutDurationMs);
                    break;
                }
                auto [err, vendor] = map_error(msg->data.error);
                cb->onError(err, vendor);
                break;
            }
            case FINGERPRINT_ACQUIRED: {
                auto [info, vendor] =
                        map_acquired(msg->data.acquired.acquired_info);
                cb->onAcquired(info, vendor);
                break;
            }
            case FINGERPRINT_TEMPLATE_ENROLLING: {
                cb->onEnrollmentProgress(
                        static_cast<int32_t>(msg->data.enroll.finger.fid),
                        static_cast<int32_t>(
                                msg->data.enroll.samples_remaining));
                break;
            }
            case FINGERPRINT_AUTHENTICATED: {
                uint32_t fid = msg->data.authenticated.finger.fid;
                if (fid != 0) {
                    {
                        std::lock_guard<std::mutex> lk(lock_);
                        failed_attempts_ = 0;
                        lockout_until_ms_ = 0;
                    }
                    ::aidl::android::hardware::keymaster::HardwareAuthToken hat;
                    to_aidl_hat(msg->data.authenticated.hat, &hat);
                    cb->onAuthenticationSucceeded(static_cast<int32_t>(fid),
                                                  hat);
                } else {
                    int32_t attempts = 0;
                    bool permanent = false;
                    bool timed = false;
                    {
                        std::lock_guard<std::mutex> lk(lock_);
                        failed_attempts_++;
                        attempts = failed_attempts_;
                        if (attempts >= kPermanentLockoutThreshold) {
                            permanent_lockout_ = true;
                            permanent = true;
                        } else if (attempts % kTimedLockoutThreshold == 0) {
                            lockout_until_ms_ = now_monotonic_ms() +
                                                kTimedLockoutDurationMs;
                            timed = true;
                        }
                    }
                    cb->onAuthenticationFailed();
                    if (permanent) {
                        if (dev_ && dev_->cancel) {
                            dev_->cancel(dev_);
                        }
                        cb->onLockoutPermanent();
                    } else if (timed) {
                        if (dev_ && dev_->cancel) {
                            dev_->cancel(dev_);
                        }
                        cb->onLockoutTimed(kTimedLockoutDurationMs);
                    }
                }
                break;
            }
            case FINGERPRINT_TEMPLATE_ENUMERATING: {
                std::vector<int32_t> done_ids;
                bool done = false;
                {
                    std::lock_guard<std::mutex> lk(lock_);
                    if (msg->data.enumerated.finger.fid != 0) {
                        enumerated_ids_.push_back(static_cast<int32_t>(
                                msg->data.enumerated.finger.fid));
                    }
                    if (msg->data.enumerated.remaining_templates == 0) {
                        done_ids.swap(enumerated_ids_);
                        done = true;
                    }
                }
                if (done) {
                    cb->onEnrollmentsEnumerated(done_ids);
                }
                break;
            }
            case FINGERPRINT_TEMPLATE_REMOVED: {
                int32_t next_fid = -1;
                std::vector<int32_t> done_removed;
                bool done = false;
                {
                    std::lock_guard<std::mutex> lk(lock_);
                    if (msg->data.removed.finger.fid != 0) {
                        removed_ids_.push_back(static_cast<int32_t>(
                                msg->data.removed.finger.fid));
                    }
                    if (msg->data.removed.remaining_templates == 0) {
                        if (!pending_remove_ids_.empty()) {
                            next_fid = pending_remove_ids_.front();
                            pending_remove_ids_.pop_front();
                        } else {
                            done_removed.swap(removed_ids_);
                            done = true;
                        }
                    }
                }
                if (next_fid >= 0) {
                    int rc = dev_->remove(dev_,
                                          static_cast<uint32_t>(user_id_),
                                          static_cast<uint32_t>(next_fid));
                    if (rc != 0) {
                        ALOGE("remove(%d) failed: %d", next_fid, rc);
                        cb->onError(Error::UNABLE_TO_REMOVE, 0);
                    }
                } else if (done) {
                    cb->onEnrollmentsRemoved(done_removed);
                }
                break;
            }
            default:
                ALOGW("Unhandled legacy fingerprint msg type=%d", msg->type);
                break;
        }
    }

    ::ndk::ScopedAStatus generateChallenge() override {
        if (!dev_ || !dev_->pre_enroll) {
            cb_->onError(Error::HW_UNAVAILABLE, 0);
            return ::ndk::ScopedAStatus::ok();
        }
        uint64_t challenge = dev_->pre_enroll(dev_);
        ALOGI("generateChallenge -> %llu",
              static_cast<unsigned long long>(challenge));
        cb_->onChallengeGenerated(static_cast<int64_t>(challenge));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus revokeChallenge(int64_t challenge) override {
        if (dev_ && dev_->post_enroll) {
            int rc = dev_->post_enroll(dev_);
            if (rc != 0) {
                ALOGW("post_enroll() returned %d", rc);
            }
        }
        cb_->onChallengeRevoked(challenge);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus enroll(
            const ::aidl::android::hardware::keymaster::HardwareAuthToken& hat,
            std::shared_ptr<common::ICancellationSignal>* out) override {
        *out = ::ndk::SharedRefBase::make<JoanCancellationSignal>(dev_);
        if (!dev_ || !dev_->enroll) {
            cb_->onError(Error::HW_UNAVAILABLE, 0);
            return ::ndk::ScopedAStatus::ok();
        }
        hw_auth_token_t legacy_hat;
        to_legacy_hat(hat, &legacy_hat);
        int rc = dev_->enroll(dev_, &legacy_hat,
                              static_cast<uint32_t>(user_id_),
                              kEnrollTimeoutSec);
        if (rc != 0) {
            ALOGE("legacy enroll() failed: %d", rc);
            cb_->onError(Error::UNABLE_TO_PROCESS, 0);
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus authenticate(
            int64_t operationId,
            std::shared_ptr<common::ICancellationSignal>* out) override {
        *out = ::ndk::SharedRefBase::make<JoanCancellationSignal>(dev_);
        if (!dev_ || !dev_->authenticate) {
            cb_->onError(Error::HW_UNAVAILABLE, 0);
            return ::ndk::ScopedAStatus::ok();
        }
        {
            std::lock_guard<std::mutex> lk(lock_);
            if (permanent_lockout_) {
                cb_->onLockoutPermanent();
                return ::ndk::ScopedAStatus::ok();
            }
            int64_t now = now_monotonic_ms();
            if (lockout_until_ms_ > now) {
                cb_->onLockoutTimed(lockout_until_ms_ - now);
                return ::ndk::ScopedAStatus::ok();
            }
        }
        int rc = dev_->authenticate(dev_, static_cast<uint64_t>(operationId),
                                    static_cast<uint32_t>(user_id_));
        if (rc != 0) {
            ALOGE("legacy authenticate() failed: %d", rc);
            cb_->onError(Error::UNABLE_TO_PROCESS, 0);
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus detectInteraction(
            std::shared_ptr<common::ICancellationSignal>* out) override {
        *out = ::ndk::SharedRefBase::make<JoanCancellationSignal>(dev_);
        cb_->onError(Error::UNABLE_TO_PROCESS, 0);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus enumerateEnrollments() override {
        if (!dev_ || !dev_->enumerate) {
            cb_->onEnrollmentsEnumerated({});
            return ::ndk::ScopedAStatus::ok();
        }
        {
            std::lock_guard<std::mutex> lk(lock_);
            enumerated_ids_.clear();
        }
        int rc = dev_->enumerate(dev_);
        if (rc != 0) {
            ALOGE("legacy enumerate() failed: %d", rc);
            cb_->onError(Error::UNABLE_TO_PROCESS, 0);
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus removeEnrollments(
            const std::vector<int32_t>& enrollmentIds) override {
        if (enrollmentIds.empty()) {
            cb_->onEnrollmentsRemoved({});
            return ::ndk::ScopedAStatus::ok();
        }
        if (!dev_ || !dev_->remove) {
            cb_->onError(Error::UNABLE_TO_REMOVE, 0);
            return ::ndk::ScopedAStatus::ok();
        }
        int32_t first_fid = 0;
        {
            std::lock_guard<std::mutex> lk(lock_);
            removed_ids_.clear();
            pending_remove_ids_.clear();
            first_fid = enrollmentIds[0];
            for (size_t i = 1; i < enrollmentIds.size(); ++i) {
                pending_remove_ids_.push_back(enrollmentIds[i]);
            }
        }
        int rc = dev_->remove(dev_, static_cast<uint32_t>(user_id_),
                              static_cast<uint32_t>(first_fid));
        if (rc != 0) {
            ALOGE("legacy remove(%d) failed: %d", first_fid, rc);
            cb_->onError(Error::UNABLE_TO_REMOVE, 0);
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getAuthenticatorId() override {
        uint64_t id = 0;
        if (dev_ && dev_->get_authenticator_id) {
            id = dev_->get_authenticator_id(dev_);
        }
        cb_->onAuthenticatorIdRetrieved(static_cast<int64_t>(id));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus invalidateAuthenticatorId() override {
        uint64_t id = 0;
        if (dev_ && dev_->get_authenticator_id) {
            id = dev_->get_authenticator_id(dev_);
        }
        cb_->onAuthenticatorIdInvalidated(static_cast<int64_t>(id));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus resetLockout(
            const ::aidl::android::hardware::keymaster::HardwareAuthToken&
            /*hat*/) override {
        {
            std::lock_guard<std::mutex> lk(lock_);
            failed_attempts_ = 0;
            lockout_until_ms_ = 0;
            permanent_lockout_ = false;
        }
        cb_->onLockoutCleared();
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus close() override {
        std::shared_ptr<ISessionCallback> cb;
        {
            std::lock_guard<std::mutex> lk(lock_);
            if (closed_) {
                return ::ndk::ScopedAStatus::ok();
            }
            closed_ = true;
            cb = cb_;
        }
        if (dev_ && dev_->cancel) {
            dev_->cancel(dev_);
        }
        if (cb) {
            cb->onSessionClosed();
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onPointerDown(int32_t, int32_t, int32_t, float,
                                       float) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onPointerUp(int32_t) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onUiReady() override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus authenticateWithContext(
            int64_t operationId, const common::OperationContext&,
            std::shared_ptr<common::ICancellationSignal>* out) override {
        return authenticate(operationId, out);
    }

    ::ndk::ScopedAStatus enrollWithContext(
            const ::aidl::android::hardware::keymaster::HardwareAuthToken& hat,
            const common::OperationContext&,
            std::shared_ptr<common::ICancellationSignal>* out) override {
        return enroll(hat, out);
    }

    ::ndk::ScopedAStatus detectInteractionWithContext(
            const common::OperationContext&,
            std::shared_ptr<common::ICancellationSignal>* out) override {
        return detectInteraction(out);
    }

    ::ndk::ScopedAStatus onPointerDownWithContext(
            const PointerContext&) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onPointerUpWithContext(
            const PointerContext&) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onContextChanged(
            const common::OperationContext&) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus onPointerCancelWithContext(
            const PointerContext&) override {
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus setIgnoreDisplayTouches(bool) override {
        return ::ndk::ScopedAStatus::ok();
    }

 private:
    int32_t sensor_id_;
    int32_t user_id_;
    std::shared_ptr<ISessionCallback> cb_;
    fingerprint_device_t* dev_;

    mutable std::mutex lock_;
    bool closed_ = false;
    int32_t failed_attempts_ = 0;
    int64_t lockout_until_ms_ = 0;
    bool permanent_lockout_ = false;
    std::vector<int32_t> enumerated_ids_;
    std::vector<int32_t> removed_ids_;
    std::deque<int32_t> pending_remove_ids_;
};

void legacy_notify_cb(const fingerprint_msg_t* msg) {
    std::shared_ptr<JoanSession> session;
    {
        std::lock_guard<std::mutex> lk(g_active_session_lock);
        session = g_active_session.lock();
    }
    if (session) {
        session->handleNotify(msg);
    }
}

class JoanFingerprint : public BnFingerprint {
 public:
    JoanFingerprint() = default;
    ~JoanFingerprint() override {
        if (dev_) {
            dev_->common.close(&dev_->common);
            dev_ = nullptr;
        }
        if (dso_) {
            dlclose(dso_);
            dso_ = nullptr;
        }
    }

    bool init() {
        wait_for_qseecomd();
        dso_ = dlopen(kFingerprintHalPath, RTLD_NOW);
        if (!dso_) {
            ALOGE("dlopen(%s) failed: %s", kFingerprintHalPath, dlerror());
            return false;
        }
        auto* mod = reinterpret_cast<hw_module_t*>(
                dlsym(dso_, HAL_MODULE_INFO_SYM_AS_STR));
        if (!mod || !mod->methods || !mod->methods->open) {
            ALOGE("HAL_MODULE_INFO_SYM missing in %s", kFingerprintHalPath);
            return false;
        }
        hw_device_t* hw = nullptr;
        int rc = mod->methods->open(mod, FINGERPRINT_HARDWARE_MODULE_ID, &hw);
        if (rc != 0 || !hw) {
            ALOGE("fingerprint open() failed: %d", rc);
            return false;
        }
        dev_ = reinterpret_cast<fingerprint_device_t*>(hw);
        rc = dev_->set_notify(dev_, legacy_notify_cb);
        if (rc != 0) {
            ALOGE("fingerprint set_notify() failed: %d", rc);
            return false;
        }
        ALOGI("Hardware TrustZone FPC1020 Fingerprint HAL ready");
        return true;
    }

    ::ndk::ScopedAStatus getSensorProps(
            std::vector<SensorProps>* out) override {
        common::CommonProps common_props;
        common_props.sensorId = kSensorId;
        common_props.sensorStrength = common::SensorStrength::STRONG;
        common_props.maxEnrollmentsPerUser = kMaxEnrollmentsPerUser;
        common_props.componentInfo = {
                {"fingerprintSensor", "fpc/fpc1020", "1.0", "", ""},
                {"matchingAlgorithm", "", "", "", "fpc/tz"},
        };

        SensorLocation loc;
        loc.display = "";

        SensorProps props;
        props.commonProps = std::move(common_props);
        props.sensorType = FingerprintSensorType::REAR;
        props.sensorLocations = {loc};
        props.supportsNavigationGestures = false;
        props.supportsDetectInteraction = false;
        props.halHandlesDisplayTouches = false;
        props.halControlsIllumination = false;

        *out = {std::move(props)};
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus createSession(
            int32_t sensorId, int32_t userId,
            const std::shared_ptr<ISessionCallback>& cb,
            std::shared_ptr<ISession>* out) override {
        if (!cb) {
            return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        std::shared_ptr<JoanSession> old_session;
        auto session = ::ndk::SharedRefBase::make<JoanSession>(sensorId, userId,
                                                               cb, dev_);
        {
            std::lock_guard<std::mutex> lk(g_active_session_lock);
            old_session = session_;
            session_ = session;
            g_active_session = session;
        }
        if (old_session && !old_session->isClosed()) {
            old_session->close();
        }
        *out = session;
        ALOGI("createSession(sensorId=%d, userId=%d) ok", sensorId, userId);
        return ::ndk::ScopedAStatus::ok();
    }

 private:
    void* dso_ = nullptr;
    fingerprint_device_t* dev_ = nullptr;
    std::shared_ptr<JoanSession> session_;
};

}  // namespace
}  // namespace aidl::android::hardware::biometrics::fingerprint

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto svc = ::ndk::SharedRefBase::make<
            aidl::android::hardware::biometrics::fingerprint::JoanFingerprint>();
    if (!svc->init()) {
        ALOGE("Failed to initialize JoanFingerprint");
        return 1;
    }
    const std::string instance =
            std::string(aidl::android::hardware::biometrics::fingerprint::
                                IFingerprint::descriptor) +
            "/default";
    binder_status_t st =
            AServiceManager_addService(svc->asBinder().get(), instance.c_str());
    if (st != STATUS_OK) {
        ALOGE("AServiceManager_addService(%s) failed: %d", instance.c_str(), st);
        return 1;
    }
    ALOGI("Registered %s", instance.c_str());
    ABinderProcess_joinThreadPool();
    return 0;
}
