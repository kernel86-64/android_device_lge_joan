// SPDX-License-Identifier: Apache-2.0
//
// Hardware AIDL IGatekeeper (V1) service wrapping Qualcomm's TrustZone
// gatekeeper.msm8998.so for LG V30 (joan).

#define LOG_TAG "android.hardware.gatekeeper-service.joan"

#include <aidl/android/hardware/gatekeeper/BnGatekeeper.h>
#include <aidl/android/hardware/security/keymint/HardwareAuthToken.h>
#include <aidl/android/hardware/security/keymint/HardwareAuthenticatorType.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <endian.h>
#include <hardware/gatekeeper.h>
#include <hardware/hardware.h>
#include <hardware/hw_auth_token.h>
#include <log/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

namespace aidl::android::hardware::gatekeeper {

namespace {

constexpr const char* kGatekeeperHalPath =
        "/vendor/lib64/hw/gatekeeper.msm8998.so";

struct __attribute__((__packed__)) LegacyPasswordHandle {
    uint8_t version;
    uint64_t user_id;
    uint64_t flags;
    uint64_t salt;
    uint8_t signature[32];
    bool hardware_backed;
};

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

void to_aidl_hat(
        const uint8_t* raw, uint32_t raw_len,
        ::aidl::android::hardware::security::keymint::HardwareAuthToken* out) {
    if (!raw || raw_len < sizeof(hw_auth_token_t)) {
        return;
    }
    const auto* tok = reinterpret_cast<const hw_auth_token_t*>(raw);
    out->challenge = static_cast<int64_t>(tok->challenge);
    out->userId = static_cast<int64_t>(tok->user_id);
    out->authenticatorId = static_cast<int64_t>(tok->authenticator_id);
    out->authenticatorType = static_cast<
            ::aidl::android::hardware::security::keymint::HardwareAuthenticatorType>(
            be32toh(tok->authenticator_type));
    out->timestamp.milliSeconds =
            static_cast<int64_t>(be64toh(tok->timestamp));
    out->mac.assign(tok->hmac, tok->hmac + sizeof(tok->hmac));
}

class JoanGatekeeper : public BnGatekeeper {
 public:
    JoanGatekeeper() = default;
    ~JoanGatekeeper() override {
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
        dso_ = dlopen(kGatekeeperHalPath, RTLD_NOW);
        if (!dso_) {
            ALOGE("dlopen(%s) failed: %s", kGatekeeperHalPath, dlerror());
            return false;
        }
        auto* mod = reinterpret_cast<hw_module_t*>(
                dlsym(dso_, HAL_MODULE_INFO_SYM_AS_STR));
        if (!mod || !mod->methods || !mod->methods->open) {
            ALOGE("HAL_MODULE_INFO_SYM missing in %s", kGatekeeperHalPath);
            return false;
        }
        hw_device_t* hw = nullptr;
        int rc = mod->methods->open(mod, HARDWARE_GATEKEEPER, &hw);
        if (rc != 0 || !hw) {
            ALOGE("gatekeeper open() failed: %d", rc);
            return false;
        }
        dev_ = reinterpret_cast<gatekeeper_device_t*>(hw);
        ALOGI("Hardware TrustZone Gatekeeper ready");
        return true;
    }

    ::ndk::ScopedAStatus enroll(
            int32_t uid, const std::vector<uint8_t>& currentPasswordHandle,
            const std::vector<uint8_t>& currentPassword,
            const std::vector<uint8_t>& desiredPassword,
            GatekeeperEnrollResponse* rsp) override {
        std::lock_guard<std::mutex> lk(lock_);
        if (!dev_ || !dev_->enroll || desiredPassword.empty()) {
            return ndk::ScopedAStatus::fromServiceSpecificError(
                    ERROR_GENERAL_FAILURE);
        }

        uint8_t* enrolled = nullptr;
        uint32_t enrolled_len = 0;
        int rc = dev_->enroll(
                dev_, static_cast<uint32_t>(uid),
                currentPasswordHandle.empty() ? nullptr
                                              : currentPasswordHandle.data(),
                static_cast<uint32_t>(currentPasswordHandle.size()),
                currentPassword.empty() ? nullptr : currentPassword.data(),
                static_cast<uint32_t>(currentPassword.size()),
                desiredPassword.data(),
                static_cast<uint32_t>(desiredPassword.size()), &enrolled,
                &enrolled_len);

        if (rc == 0 && enrolled && enrolled_len >= sizeof(LegacyPasswordHandle)) {
            const auto* ph =
                    reinterpret_cast<const LegacyPasswordHandle*>(enrolled);
            rsp->statusCode = STATUS_OK;
            rsp->timeoutMs = 0;
            rsp->secureUserId = static_cast<int64_t>(ph->user_id);
            rsp->data.assign(enrolled, enrolled + enrolled_len);
            free(enrolled);
            return ndk::ScopedAStatus::ok();
        }
        free(enrolled);
        if (rc > 0) {
            rsp->statusCode = ERROR_RETRY_TIMEOUT;
            rsp->timeoutMs = rc;
            rsp->secureUserId = 0;
            rsp->data.clear();
            return ndk::ScopedAStatus::ok();
        }
        ALOGE("gatekeeper enroll(uid=%d) failed: %d", uid, rc);
        return ndk::ScopedAStatus::fromServiceSpecificError(
                ERROR_GENERAL_FAILURE);
    }

    ::ndk::ScopedAStatus verify(
            int32_t uid, int64_t challenge,
            const std::vector<uint8_t>& enrolledPasswordHandle,
            const std::vector<uint8_t>& providedPassword,
            GatekeeperVerifyResponse* rsp) override {
        std::lock_guard<std::mutex> lk(lock_);
        if (!dev_ || !dev_->verify || enrolledPasswordHandle.empty()) {
            return ndk::ScopedAStatus::fromServiceSpecificError(
                    ERROR_GENERAL_FAILURE);
        }

        uint8_t* hat = nullptr;
        uint32_t hat_len = 0;
        bool req_reenroll = false;
        int rc = dev_->verify(
                dev_, static_cast<uint32_t>(uid),
                static_cast<uint64_t>(challenge),
                enrolledPasswordHandle.data(),
                static_cast<uint32_t>(enrolledPasswordHandle.size()),
                providedPassword.empty() ? nullptr : providedPassword.data(),
                static_cast<uint32_t>(providedPassword.size()), &hat, &hat_len,
                &req_reenroll);

        if (rc == 0) {
            rsp->statusCode = req_reenroll ? STATUS_REENROLL : STATUS_OK;
            rsp->timeoutMs = 0;
            to_aidl_hat(hat, hat_len, &rsp->hardwareAuthToken);
            free(hat);
            return ndk::ScopedAStatus::ok();
        }
        free(hat);
        if (rc > 0) {
            rsp->statusCode = ERROR_RETRY_TIMEOUT;
            rsp->timeoutMs = rc;
            return ndk::ScopedAStatus::ok();
        }
        return ndk::ScopedAStatus::fromServiceSpecificError(
                ERROR_GENERAL_FAILURE);
    }

    ::ndk::ScopedAStatus deleteUser(int32_t uid) override {
        std::lock_guard<std::mutex> lk(lock_);
        if (!dev_ || !dev_->delete_user) {
            return ndk::ScopedAStatus::fromServiceSpecificError(
                    ERROR_NOT_IMPLEMENTED);
        }
        int rc = dev_->delete_user(dev_, static_cast<uint32_t>(uid));
        return rc == 0 ? ndk::ScopedAStatus::ok()
                       : ndk::ScopedAStatus::fromServiceSpecificError(
                                 ERROR_GENERAL_FAILURE);
    }

    ::ndk::ScopedAStatus deleteAllUsers() override {
        std::lock_guard<std::mutex> lk(lock_);
        if (!dev_ || !dev_->delete_all_users) {
            return ndk::ScopedAStatus::fromServiceSpecificError(
                    ERROR_NOT_IMPLEMENTED);
        }
        int rc = dev_->delete_all_users(dev_);
        return rc == 0 ? ndk::ScopedAStatus::ok()
                       : ndk::ScopedAStatus::fromServiceSpecificError(
                                 ERROR_GENERAL_FAILURE);
    }

 private:
    std::mutex lock_;
    void* dso_ = nullptr;
    gatekeeper_device_t* dev_ = nullptr;
};

}  // namespace
}  // namespace aidl::android::hardware::gatekeeper

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto svc = ndk::SharedRefBase::make<
            aidl::android::hardware::gatekeeper::JoanGatekeeper>();
    if (!svc->init()) {
        ALOGE("Failed to initialize JoanGatekeeper");
        return 1;
    }
    const std::string instance =
            std::string(aidl::android::hardware::gatekeeper::IGatekeeper::descriptor) +
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
