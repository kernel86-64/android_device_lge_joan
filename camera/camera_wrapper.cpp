// SPDX-License-Identifier: Apache-2.0
//
// Camera3 HAL wrapper around camera.libcamera.so for LG V30 (joan).
//
// 1. Orders internal cameras deterministically regardless of kernel probe order:
//      0: Back Main  (Sony IMX351,    4656x3496, 16 MP, 4.03 mm, f/1.6)
//      1: Front      (Hynix HI553,    2560x1920,  5 MP, 2.46 mm, f/2.2)
//      2: Back Wide  (Samsung S5K3M3, 4192x3128, 13 MP, 1.52 mm, f/1.9)
// 2. Populates real LG V30 focal lengths and apertures in static characteristics.
// 3. Drives the ROHM BU24235 VCM/OIS actuator (/dev/v4l-subdev* "bu24235*")
//    for Camera 0: manual focus (ANDROID_LENS_FOCUS_DISTANCE) and contrast-
//    detection autofocus (CDAF) for tap-to-focus (AUTO/MACRO) and continuous AF.
// 4. Tracks in-flight camera3 requests and drains any requests left behind by
//    camera.libcamera.so on flush() to avoid 5s waitUntilDrainedLocked timeouts
//    when switching cameras.

#define LOG_TAG "camera.joan"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <hardware/camera3.h>
#include <hardware/camera_common.h>
#include <hardware/hardware.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char* kLibcameraHalPath = "/vendor/lib64/hw/camera.libcamera.so";
constexpr float kMinFocusDiopters = 10.0f;  // 10 cm macro
constexpr float kHyperfocalDiopters = 0.25f;
constexpr int kVcmMinPos = 0;
constexpr int kVcmMaxPos = 1023;
constexpr int kVcmMacroPos = 420;
constexpr int kVcmInfPos = 1023;
constexpr int kVcmDefaultPos = 960;
constexpr int kPipelineSkipFrames = 3;  // VFE + SoftISP latency after VCM stops

using SetTorchStrengthFn = int (*)(const char*, int);
using GetTorchStrengthFn = int (*)(const char*, int*);

struct CameraEntry {
    int raw_id = -1;
    int priority = 99;
    bool has_vcm = false;
    int32_t active_w = 0;
    int32_t active_h = 0;
    float focal_length = 0.0f;
    float aperture = 0.0f;
    camera_info info{};
    camera_metadata_t* patched_meta = nullptr;
};

std::mutex g_lock;
void* g_dso = nullptr;
camera_module_t* g_libcam = nullptr;
SetTorchStrengthFn g_set_torch_strength = nullptr;
GetTorchStrengthFn g_get_torch_strength = nullptr;
const camera_module_callbacks_t* g_fw_callbacks = nullptr;
bool g_mapped = false;
std::string g_vcm_dev_path;
std::string g_tof_sysfs_path;
std::vector<CameraEntry> g_cameras;  // indexed by external camera id

// ---- VCM (ROHM BU24235) & ToF (ST VL53L0X) discovery ----

std::string find_bu24235_subdev() {
    DIR* dir = opendir("/sys/class/video4linux");
    if (!dir) {
        return {};
    }
    std::string found;
    while (struct dirent* ent = readdir(dir)) {
        if (strncmp(ent->d_name, "v4l-subdev", 10) != 0) {
            continue;
        }
        char name_path[128];
        snprintf(name_path, sizeof(name_path),
                 "/sys/class/video4linux/%s/name", ent->d_name);
        FILE* fp = fopen(name_path, "r");
        if (!fp) {
            continue;
        }
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), fp) && strncmp(buf, "bu24235", 7) == 0) {
            found = std::string("/dev/") + ent->d_name;
        }
        fclose(fp);
        if (!found.empty()) {
            break;
        }
    }
    closedir(dir);
    return found;
}

std::string find_vl53l0x_sysfs() {
    DIR* dir = opendir("/sys/bus/iio/devices");
    if (!dir) {
        return {};
    }
    std::string found;
    while (struct dirent* ent = readdir(dir)) {
        if (strncmp(ent->d_name, "iio:device", 10) != 0) {
            continue;
        }
        char name_path[160];
        snprintf(name_path, sizeof(name_path),
                 "/sys/bus/iio/devices/%s/name", ent->d_name);
        FILE* fp = fopen(name_path, "r");
        if (!fp) {
            continue;
        }
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), fp) && strncmp(buf, "vl53l0x", 7) == 0) {
            found = std::string("/sys/bus/iio/devices/") + ent->d_name +
                    "/in_distance_raw";
        }
        fclose(fp);
        if (!found.empty()) {
            break;
        }
    }
    closedir(dir);
    return found;
}

// ---- Camera metadata helpers ----

camera_metadata_t* copy_and_grow_metadata(const camera_metadata_t* src,
                                          size_t extra_entries,
                                          size_t extra_bytes) {
    if (!src) {
        return nullptr;
    }
    const size_t entries = get_camera_metadata_entry_count(src) + extra_entries;
    const size_t data = get_camera_metadata_data_count(src) + extra_bytes;
    camera_metadata_t* dst = allocate_camera_metadata(entries, data);
    if (!dst) {
        return nullptr;
    }
    if (append_camera_metadata(dst, src) != 0) {
        free_camera_metadata(dst);
        return nullptr;
    }
    return dst;
}

template <typename T>
void set_or_add_entry(camera_metadata_t* meta, uint32_t tag,
                      const T* data, size_t count) {
    camera_metadata_entry_t entry{};
    if (find_camera_metadata_entry(meta, tag, &entry) == 0) {
        update_camera_metadata_entry(meta, entry.index, data, count, nullptr);
    } else {
        add_camera_metadata_entry(meta, tag, data, count);
    }
}

void append_unique_i32_keys(camera_metadata_t* meta, uint32_t tag,
                            const std::vector<int32_t>& keys_to_add) {
    camera_metadata_entry_t entry{};
    if (find_camera_metadata_entry(meta, tag, &entry) != 0 ||
        entry.type != TYPE_INT32) {
        return;
    }
    std::vector<int32_t> vals(entry.data.i32, entry.data.i32 + entry.count);
    for (int32_t k : keys_to_add) {
        if (std::find(vals.begin(), vals.end(), k) == vals.end()) {
            vals.push_back(k);
        }
    }
    update_camera_metadata_entry(meta, entry.index, vals.data(), vals.size(),
                                 nullptr);
}

// ---- Contrast-detection AF + Manual Focus controller ----

struct AfRegionNorm {
    float x0 = 0.35f;
    float y0 = 0.35f;
    float x1 = 0.65f;
    float y1 = 0.65f;
    int32_t raw[5] = {0, 0, 0, 0, 0};
    bool has_raw = false;
};

int diopters_to_vcm(float diopters) {
    float t = std::clamp(diopters / kMinFocusDiopters, 0.0f, 1.0f);
    // BU24235: 1023 is infinity (0.0 D), 520 is macro (10.0 D).
    int pos = static_cast<int>(
            std::lround(kVcmInfPos - t * (kVcmInfPos - kVcmMacroPos)));
    return std::clamp(pos, kVcmMinPos, kVcmMaxPos);
}

float vcm_to_diopters(int pos) {
    float t = static_cast<float>(kVcmInfPos - pos) /
              static_cast<float>(kVcmInfPos - kVcmMacroPos);
    return std::clamp(t, 0.0f, 1.0f) * kMinFocusDiopters;
}

struct FrameMetrics {
    double sharpness = 0.0;
    double mean_luma = 0.0;
    bool valid = false;
};

// Compute Tenengrad contrast sharpness on the linear Y plane of a Qualcomm
// msm8998 gralloc1 NV12/YCbCr_420_888 buffer (fast: <0.2 ms via strided scan).
FrameMetrics compute_frame_sharpness(buffer_handle_t handle,
                                     const AfRegionNorm& roi) {
    static bool logged_once = false;
    FrameMetrics out{};
    if (!handle || handle->numFds < 1 || handle->numInts < 12 ||
        handle->data[2] != 0x676d736d) {
        if (!logged_once && handle) {
            logged_once = true;
            ALOGW("AF buffer rejected: numFds=%d numInts=%d magic=0x%x",
                  handle->numFds, handle->numInts,
                  handle->numInts > 0 ? handle->data[handle->numFds] : 0);
        }
        return out;
    }
    const int fd = handle->data[0];
    const uint32_t flags = static_cast<uint32_t>(handle->data[3]);
    const int stride = handle->data[4];
    const int width = handle->data[6];
    const int height = handle->data[7];
    const int format = handle->data[8];
    const int size = handle->data[10];
    const int offset = handle->data[11];

    if (!logged_once) {
        logged_once = true;
        ALOGI("AF buffer info: fd=%d flags=0x%x stride=%d %dx%d fmt=0x%x size=%d off=%d",
              fd, flags, stride, width, height, format, size, offset);
    }

    // Skip UBWC-compressed or secure buffers; libcamera SoftISP outputs linear.
    constexpr uint32_t kFlagUbwcOrSecure = 0x08000000u | 0x00000400u;
    if ((flags & kFlagUbwcOrSecure) != 0 || fd < 0 || stride <= 0 ||
        width <= 16 || height <= 16 || size <= 0 || offset < 0) {
        return out;
    }

    int bpp = 1;  // Y plane for YUV/NV12/YCbCr_420_888/IMPLEMENTATION_DEFINED
    if (format == HAL_PIXEL_FORMAT_RGBA_8888 ||
        format == HAL_PIXEL_FORMAT_RGBX_8888 ||
        format == HAL_PIXEL_FORMAT_BGRA_8888) {
        bpp = 4;
    } else if (format == HAL_PIXEL_FORMAT_BLOB) {
        return out;
    }

    const size_t row_bytes = static_cast<size_t>(stride) * bpp;
    const size_t needed = static_cast<size_t>(offset) +
                          row_bytes * static_cast<size_t>(height);
    if (needed > static_cast<size_t>(size)) {
        return out;
    }

    struct dma_buf_sync sync_start = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
    };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_start);

    void* map = mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_SHARED,
                     fd, 0);
    if (map == MAP_FAILED) {
        struct dma_buf_sync sync_end = {
            .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ,
        };
        ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_end);
        return out;
    }

    const uint8_t* base = static_cast<const uint8_t*>(map) + offset;
    int x0 = std::clamp(static_cast<int>(roi.x0 * width), 4, width - 8);
    int y0 = std::clamp(static_cast<int>(roi.y0 * height), 4, height - 8);
    int x1 = std::clamp(static_cast<int>(roi.x1 * width), x0 + 8, width - 4);
    int y1 = std::clamp(static_cast<int>(roi.y1 * height), y0 + 8, height - 4);

    // Step 2 pixels with central 1px differences (row[x+1]-row[x-1],
    // row_dn[x]-row_up[x]) to measure true fine detail, normalized by mean
    // luma^2 so SoftISP AGC brightness changes never fool contrast AF.
    constexpr int kStep = 2;
    double grad_sum = 0.0;
    double luma_sum = 0.0;
    uint64_t samples = 0;

    if (bpp == 1) {
        for (int y = y0; y < y1; y += kStep) {
            const uint8_t* row_up =
                    base + static_cast<size_t>(y - 1) * row_bytes;
            const uint8_t* row = base + static_cast<size_t>(y) * row_bytes;
            const uint8_t* row_dn =
                    base + static_cast<size_t>(y + 1) * row_bytes;
            for (int x = x0; x < x1; x += kStep) {
                int gx = static_cast<int>(row[x + 1]) -
                         static_cast<int>(row[x - 1]);
                int gy = static_cast<int>(row_dn[x]) -
                         static_cast<int>(row_up[x]);
                grad_sum += static_cast<double>(gx * gx + gy * gy);
                luma_sum += static_cast<double>(row[x]);
                ++samples;
            }
        }
    } else {
        // RGBA: use green channel (offset 1) as luma proxy.
        for (int y = y0; y < y1; y += kStep) {
            const uint8_t* row_up =
                    base + static_cast<size_t>(y - 1) * row_bytes;
            const uint8_t* row = base + static_cast<size_t>(y) * row_bytes;
            const uint8_t* row_dn =
                    base + static_cast<size_t>(y + 1) * row_bytes;
            for (int x = x0; x < x1; x += kStep) {
                int gx = static_cast<int>(row[(x + 1) * 4 + 1]) -
                         static_cast<int>(row[(x - 1) * 4 + 1]);
                int gy = static_cast<int>(row_dn[x * 4 + 1]) -
                         static_cast<int>(row_up[x * 4 + 1]);
                grad_sum += static_cast<double>(gx * gx + gy * gy);
                luma_sum += static_cast<double>(row[x * 4 + 1]);
                ++samples;
            }
        }
    }

    munmap(map, static_cast<size_t>(size));
    struct dma_buf_sync sync_end = {
        .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ,
    };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_end);

    if (samples > 0) {
        double mean = luma_sum / static_cast<double>(samples);
        double norm = (mean * mean) + 256.0;
        out.mean_luma = mean;
        // Scale by 1000 for readable scores (typically 10..2000).
        out.sharpness = (grad_sum / static_cast<double>(samples)) * 1000.0 / norm;
        out.valid = true;
    }
    return out;
}

class AfController {
 public:
    ~AfController() { close(); }

    void init(int vcm_fd, int32_t active_w, int32_t active_h,
              const std::string& tof_path) {
        {
            std::lock_guard<std::mutex> guard(lock_);
            vcm_fd_ = vcm_fd;
            tof_path_ = tof_path;
            active_w_ = active_w > 0 ? active_w : 4656;
            active_h_ = active_h > 0 ? active_h : 3496;
            af_mode_ = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
            af_state_ = ANDROID_CONTROL_AF_STATE_INACTIVE;
            lens_state_ = ANDROID_LENS_STATE_STATIONARY;
            current_pos_ = kVcmDefaultPos;
            target_pos_ = kVcmDefaultPos;
            hw_pos_ = kVcmDefaultPos;
            vcm_moving_ = true;
            vcm_stop_ = false;
            tof_stop_ = false;
            tof_mm_ = -1;
            tof_seq_ = 0;
            last_checked_tof_seq_ = 0;
            locked_tof_mm_ = -1;
            tof_change_streak_ = 0;
            tof_vcm_offset_ = 0;
            tof_cal_samples_ = 0;
            phase_ = Phase::kIdle;
            initial_scan_done_ = false;
        }
        vcm_thread_ = std::thread([this]() { vcm_worker(); });
        if (!tof_path.empty()) {
            tof_thread_ = std::thread([this]() { tof_worker(); });
        }
    }

    void close() {
        {
            std::lock_guard<std::mutex> guard(lock_);
            vcm_stop_ = true;
            tof_stop_ = true;
        }
        vcm_cv_.notify_all();
        tof_cv_.notify_all();
        if (vcm_thread_.joinable()) {
            vcm_thread_.join();
        }
        if (tof_thread_.joinable()) {
            tof_thread_.join();
        }
        std::lock_guard<std::mutex> guard(lock_);
        if (vcm_fd_ >= 0) {
            ::close(vcm_fd_);
            vcm_fd_ = -1;
        }
    }

    void on_request(const camera_metadata_t* settings) {
        std::lock_guard<std::mutex> guard(lock_);
        if (vcm_fd_ < 0) {
            return;
        }

        uint8_t trigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
        bool has_manual_dist = false;
        float manual_dist = 0.0f;

        if (settings) {
            camera_metadata_ro_entry_t e{};
            if (find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_AF_MODE,
                                              &e) == 0 &&
                e.count >= 1) {
                uint8_t new_mode = e.data.u8[0];
                if (new_mode != af_mode_) {
                    uint8_t prev_mode = af_mode_;
                    af_mode_ = new_mode;
                    if (af_mode_ == ANDROID_CONTROL_AF_MODE_OFF) {
                        phase_ = Phase::kIdle;
                        af_state_ = ANDROID_CONTROL_AF_STATE_INACTIVE;
                        lens_state_ = ANDROID_LENS_STATE_STATIONARY;
                    } else if (af_mode_ ==
                                       ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE ||
                               af_mode_ ==
                                       ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO) {
                        // Don't immediately discard a tap-to-focus lock when
                        // CameraX returns from AUTO -> CONTINUOUS; keep the
                        // locked position and let the scene monitor take over.
                        if (prev_mode == ANDROID_CONTROL_AF_MODE_AUTO ||
                            prev_mode == ANDROID_CONTROL_AF_MODE_MACRO) {
                            phase_ = Phase::kIdle;
                            lens_state_ = ANDROID_LENS_STATE_STATIONARY;
                            af_state_ = ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED;
                            monitor_counter_ = -30;
                        } else if (!initial_scan_done_) {
                            start_scan_locked(/*active=*/false);
                        }
                    } else {
                        phase_ = Phase::kIdle;
                        af_state_ = ANDROID_CONTROL_AF_STATE_INACTIVE;
                    }
                }
            }
            if (find_camera_metadata_ro_entry(
                        settings, ANDROID_CONTROL_AF_TRIGGER, &e) == 0 &&
                e.count >= 1) {
                trigger = e.data.u8[0];
            }
            if (find_camera_metadata_ro_entry(
                        settings, ANDROID_LENS_FOCUS_DISTANCE, &e) == 0 &&
                e.count >= 1) {
                has_manual_dist = true;
                manual_dist = e.data.f[0];
            }
            if (find_camera_metadata_ro_entry(
                        settings, ANDROID_CONTROL_AF_REGIONS, &e) == 0 &&
                e.count >= 5) {
                int32_t x0 = e.data.i32[0];
                int32_t y0 = e.data.i32[1];
                int32_t x1 = e.data.i32[2];
                int32_t y1 = e.data.i32[3];
                int32_t wt = e.data.i32[4];
                for (int i = 0; i < 5; ++i) roi_.raw[i] = e.data.i32[i];
                roi_.has_raw = true;
                if (wt > 0 && x1 > x0 && y1 > y0) {
                    roi_.x0 = std::clamp(
                            static_cast<float>(x0) / active_w_, 0.05f, 0.85f);
                    roi_.y0 = std::clamp(
                            static_cast<float>(y0) / active_h_, 0.05f, 0.85f);
                    roi_.x1 = std::clamp(
                            static_cast<float>(x1) / active_w_, roi_.x0 + 0.1f,
                            0.95f);
                    roi_.y1 = std::clamp(
                            static_cast<float>(y1) / active_h_, roi_.y0 + 0.1f,
                            0.95f);
                } else {
                    roi_.x0 = 0.35f;
                    roi_.y0 = 0.35f;
                    roi_.x1 = 0.65f;
                    roi_.y1 = 0.65f;
                }
            }
        }

        if (af_mode_ == ANDROID_CONTROL_AF_MODE_OFF) {
            if (has_manual_dist) {
                int target = diopters_to_vcm(manual_dist);
                if (target != current_pos_) {
                    ALOGI("MF: %.2fD -> pos=%d", manual_dist, target);
                    set_vcm_locked(target);
                }
            }
            af_state_ = ANDROID_CONTROL_AF_STATE_INACTIVE;
            lens_state_ = ANDROID_LENS_STATE_STATIONARY;
            return;
        }

        if (trigger == ANDROID_CONTROL_AF_TRIGGER_START) {
            start_scan_locked(/*active=*/true);
        } else if (trigger == ANDROID_CONTROL_AF_TRIGGER_CANCEL) {
            phase_ = Phase::kIdle;
            lens_state_ = ANDROID_LENS_STATE_STATIONARY;
            af_state_ = (af_mode_ == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE ||
                         af_mode_ == ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                                ? ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED
                                : ANDROID_CONTROL_AF_STATE_INACTIVE;
        } else if (!initial_scan_done_ && phase_ == Phase::kIdle &&
                   (af_mode_ == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE ||
                    af_mode_ == ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO)) {
            start_scan_locked(/*active=*/false);
        }
    }

    void on_preview_frame(buffer_handle_t handle) {
        AfRegionNorm roi_copy;
        bool need_metrics = false;
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (vcm_fd_ < 0 || af_mode_ == ANDROID_CONTROL_AF_MODE_OFF) {
                return;
            }
            if (phase_ != Phase::kIdle) {
                if (vcm_moving_) {
                    return;
                }
                if (skip_frames_ > 0) {
                    --skip_frames_;
                    return;
                }
                need_metrics = true;
            } else if (af_mode_ == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE ||
                       af_mode_ == ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO) {
                if (!vcm_moving_) {
                    ++monitor_counter_;
                    // Fast laser distance-change trigger in continuous AF once
                    // any post-tap cooldown has elapsed.
                    if (monitor_counter_ >= 0 &&
                        tof_seq_ != last_checked_tof_seq_ &&
                        roi_is_near_center_locked()) {
                        last_checked_tof_seq_ = tof_seq_;
                        int cur_mm = fresh_tof_mm_locked();
                        if (cur_mm >= 60 && locked_tof_mm_ >= 60) {
                            float d_now = tof_mm_to_diopters(cur_mm);
                            float d_lock = tof_mm_to_diopters(locked_tof_mm_);
                            if (std::fabs(d_now - d_lock) > 0.70f) {
                                if (++tof_change_streak_ >= 2) {
                                    ALOGI("AF laser trigger: %dmm -> %dmm "
                                          "(dD=%.2f)",
                                          locked_tof_mm_, cur_mm,
                                          std::fabs(d_now - d_lock));
                                    tof_change_streak_ = 0;
                                    monitor_counter_ = 0;
                                    start_scan_locked(/*active=*/false);
                                    return;
                                }
                            } else {
                                tof_change_streak_ = 0;
                            }
                        } else {
                            tof_change_streak_ = 0;
                        }
                    }
                    if (monitor_counter_ >= 25) {
                        monitor_counter_ = 0;
                        need_metrics = true;
                    }
                }
            }
            roi_copy = roi_;
        }

        if (!need_metrics) {
            return;
        }

        // Compute sharpness outside lock_ so request threads never stall.
        FrameMetrics m = compute_frame_sharpness(handle, roi_copy);
        if (!m.valid) {
            return;
        }

        std::lock_guard<std::mutex> guard(lock_);
        if (vcm_fd_ < 0 || af_mode_ == ANDROID_CONTROL_AF_MODE_OFF) {
            return;
        }

        if (phase_ == Phase::kIdle) {
            // Scene-change monitor for continuous AF: re-scan only on clear
            // defocus or large scene shift to avoid hunting.
            if (locked_sharpness_ > 3.0) {
                double ratio = m.sharpness / locked_sharpness_;
                double luma_diff = std::fabs(m.mean_luma - locked_luma_);
                if (ratio < 0.55 || ratio > 1.85 || luma_diff > 32.0) {
                    start_scan_locked(/*active=*/false);
                }
            }
            return;
        }

        if (phase_ == Phase::kCoarse) {
            coarse_scores_.push_back(m.sharpness);
            if (m.sharpness >= best_score_) {
                best_score_ = m.sharpness;
                best_pos_ = current_pos_;
                best_coarse_idx_ = step_idx_;
            }
            ++step_idx_;
            // Early-stop once we clearly passed a real sharpness peak.
            const bool passed_peak =
                    (best_score_ > 5.0) &&
                    (step_idx_ >= best_coarse_idx_ + 2) &&
                    (m.sharpness < best_score_ * 0.55);
            if (!passed_peak && step_idx_ < coarse_steps_.size()) {
                set_vcm_locked(coarse_steps_[step_idx_]);
                skip_frames_ = kPipelineSkipFrames;
            } else {
                std::string trace;
                for (size_t i = 0; i < coarse_scores_.size() &&
                                   i < coarse_steps_.size();
                     ++i) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d:%.1f ", coarse_steps_[i],
                             coarse_scores_[i]);
                    trace += buf;
                }
                ALOGI("AF coarse: %s-> best=%d (%.1f)", trace.c_str(),
                      best_pos_, best_score_);

                // 4-point fine sweep around coarse peak (+/-20, +/-40) for tight
                // focus accuracy (~+/-10 DAC).
                coarse_best_pos_ = best_pos_;
                coarse_best_score_ = best_score_;
                fine_steps_.clear();
                const int offsets[] = {-40, -20, 20, 40};
                for (int off : offsets) {
                    int p = std::clamp(coarse_best_pos_ + off, kVcmMacroPos,
                                       kVcmInfPos);
                    if (p != coarse_best_pos_ &&
                        std::find(fine_steps_.begin(), fine_steps_.end(), p) ==
                                fine_steps_.end()) {
                        fine_steps_.push_back(p);
                    }
                }
                if (!fine_steps_.empty() &&
                    std::abs(current_pos_ - fine_steps_.back()) <
                            std::abs(current_pos_ - fine_steps_.front())) {
                    std::reverse(fine_steps_.begin(), fine_steps_.end());
                }
                fine_scores_.clear();
                phase_ = Phase::kFine;
                step_idx_ = 0;
                set_vcm_locked(fine_steps_[0]);
                skip_frames_ = kPipelineSkipFrames;
            }
            return;
        }

        if (phase_ == Phase::kFine) {
            fine_scores_.push_back(m.sharpness);
            if (m.sharpness > best_score_) {
                best_score_ = m.sharpness;
                best_pos_ = current_pos_;
            }
            ++step_idx_;
            if (step_idx_ < fine_steps_.size()) {
                set_vcm_locked(fine_steps_[step_idx_]);
                skip_frames_ = kPipelineSkipFrames;
            } else {
                // Collect (pos, score) pairs sorted by position and pick peak
                // with quadratic interpolation around the highest interior point.
                std::vector<std::pair<int, double>> pts;
                pts.reserve(fine_scores_.size() + 1);
                pts.emplace_back(coarse_best_pos_, coarse_best_score_);
                for (size_t i = 0; i < fine_steps_.size() &&
                                   i < fine_scores_.size();
                     ++i) {
                    pts.emplace_back(fine_steps_[i], fine_scores_[i]);
                }
                std::sort(pts.begin(), pts.end());

                std::string ftrace;
                size_t best_i = 0;
                for (size_t i = 0; i < pts.size(); ++i) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d:%.1f ", pts[i].first,
                             pts[i].second);
                    ftrace += buf;
                    if (pts[i].second > pts[best_i].second) {
                        best_i = i;
                    }
                }
                ALOGI("AF fine: %s", ftrace.c_str());

                int final_pos = pts[best_i].first;
                best_score_ = pts[best_i].second;
                if (best_i > 0 && best_i + 1 < pts.size()) {
                    int x0 = pts[best_i - 1].first;
                    int x1 = pts[best_i].first;
                    int x2 = pts[best_i + 1].first;
                    double y0 = pts[best_i - 1].second;
                    double y1 = pts[best_i].second;
                    double y2 = pts[best_i + 1].second;
                    if (x0 < x1 && x1 < x2) {
                        double dx0 = static_cast<double>(x0 - x1);
                        double dx2 = static_cast<double>(x2 - x1);
                        double d0 = (y0 - y1) / dx0;
                        double d2 = (y2 - y1) / dx2;
                        double a = (d2 - d0) / (dx2 - dx0);
                        double b = d0 - a * dx0;
                        if (a < -1e-6) {
                            double delta = std::clamp(-b / (2.0 * a), dx0, dx2);
                            final_pos = std::clamp(
                                    x1 + static_cast<int>(std::lround(delta)),
                                    kVcmMacroPos, kVcmInfPos);
                        }
                    }
                }
                set_vcm_locked(final_pos);
                locked_sharpness_ = best_score_;
                locked_luma_ = m.mean_luma;
                locked_tof_mm_ = fresh_tof_mm_locked();
                tof_change_streak_ = 0;

                // Online self-calibration of VL53L0X -> BU24235 offset when
                // fine CDAF found a clean interior sharpness peak.
                if (scan_used_tof_ && scan_tof_mm_ >= 80 &&
                    scan_tof_mm_ <= 1200 && best_i > 0 &&
                    best_i + 1 < pts.size() && best_score_ > 5.0) {
                    int raw_est = diopters_to_vcm(1000.0f / scan_tof_mm_);
                    int err = std::clamp(final_pos - raw_est, -120, 120);
                    if (tof_cal_samples_ == 0) {
                        tof_vcm_offset_ = err / 2;
                    } else {
                        tof_vcm_offset_ = (tof_vcm_offset_ * 3 + err) / 4;
                    }
                    ++tof_cal_samples_;
                }

                phase_ = Phase::kIdle;
                lens_state_ = ANDROID_LENS_STATE_STATIONARY;
                initial_scan_done_ = true;
                monitor_counter_ = active_scan_ ? -40 : 0;
                af_state_ = active_scan_
                                    ? ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED
                                    : ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED;
                ALOGI("AF converged: pos=%d (%.2fD) score=%.1f active=%d "
                      "tof=%dmm off=%d",
                      final_pos, vcm_to_diopters(final_pos), best_score_,
                      active_scan_ ? 1 : 0, locked_tof_mm_, tof_vcm_offset_);
            }
        }
    }

    void get_result_state(uint8_t* af_mode, uint8_t* af_state,
                          uint8_t* lens_state, float* focus_dist,
                          AfRegionNorm* roi) {
        std::lock_guard<std::mutex> guard(lock_);
        *af_mode = af_mode_;
        *af_state = af_state_;
        *lens_state = lens_state_;
        *focus_dist = vcm_to_diopters(current_pos_);
        *roi = roi_;
    }

 private:
    enum class Phase { kIdle, kCoarse, kFine };

    void vcm_worker() {
        bool first_write = true;
        while (true) {
            int fd = -1;
            int goal = 0;
            int cur = 0;
            {
                std::unique_lock<std::mutex> lk(lock_);
                vcm_cv_.wait(lk, [&]() {
                    return vcm_stop_ || first_write || (hw_pos_ != target_pos_);
                });
                if (vcm_stop_) {
                    return;
                }
                fd = vcm_fd_;
                goal = target_pos_;
                cur = hw_pos_;
            }

            if (fd < 0) {
                first_write = false;
                continue;
            }

            constexpr int kMaxStep = 28;
            if (first_write) {
                first_write = false;
                struct v4l2_control ctrl = {
                    .id = V4L2_CID_FOCUS_ABSOLUTE,
                    .value = goal,
                };
                if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) != 0) {
                    ALOGW("VIDIOC_S_CTRL(init=%d) failed: %s", goal,
                          strerror(errno));
                }
                std::lock_guard<std::mutex> lk(lock_);
                hw_pos_ = goal;
                if (hw_pos_ == target_pos_) {
                    vcm_moving_ = false;
                }
                continue;
            }

            while (cur != goal) {
                int diff = goal - cur;
                int next = (std::abs(diff) <= kMaxStep)
                                   ? goal
                                   : cur + ((diff > 0) ? kMaxStep : -kMaxStep);
                struct v4l2_control ctrl = {
                    .id = V4L2_CID_FOCUS_ABSOLUTE,
                    .value = next,
                };
                if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) != 0) {
                    ALOGW("VIDIOC_S_CTRL(focus=%d) failed: %s", next,
                          strerror(errno));
                    break;
                }
                cur = next;
                {
                    std::lock_guard<std::mutex> lk(lock_);
                    hw_pos_ = cur;
                    if (vcm_stop_) {
                        return;
                    }
                    goal = target_pos_;
                }
                if (cur != goal) {
                    usleep(800);
                }
            }

            std::lock_guard<std::mutex> lk(lock_);
            if (hw_pos_ == target_pos_) {
                vcm_moving_ = false;
            }
        }
    }

    // Never sleep on libcamera's callback/request threads; hand target off to
    // the dedicated VCM ramp thread instead.
    void set_vcm_locked(int target) {
        target = std::clamp(target, kVcmMinPos, kVcmMaxPos);
        current_pos_ = target;
        target_pos_ = target;
        if (vcm_fd_ >= 0) {
            vcm_moving_ = (hw_pos_ != target_pos_);
            vcm_cv_.notify_one();
        }
    }

    static float tof_mm_to_diopters(int mm) {
        if (mm >= 1400) {
            return 0.0f;
        }
        if (mm < 60) {
            return 0.0f;
        }
        return std::clamp(1000.0f / static_cast<float>(mm), 0.0f,
                          kMinFocusDiopters);
    }

    bool roi_is_near_center_locked() const {
        float cx = 0.5f * (roi_.x0 + roi_.x1);
        float cy = 0.5f * (roi_.y0 + roi_.y1);
        return std::fabs(cx - 0.5f) <= 0.22f && std::fabs(cy - 0.5f) <= 0.22f;
    }

    // Returns fresh laser distance in mm (>=60) if valid and <700ms old, else -1.
    int fresh_tof_mm_locked() const {
        if (tof_mm_ < 60) {
            return -1;
        }
        auto age = std::chrono::steady_clock::now() - tof_ts_;
        if (age > std::chrono::milliseconds(700)) {
            return -1;
        }
        return tof_mm_;
    }

    void tof_worker() {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(lock_);
            path = tof_path_;
        }
        while (true) {
            int mm = -1;
            int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                char buf[32] = {};
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                ::close(fd);
                if (n > 0) {
                    char* end = nullptr;
                    long v = strtol(buf, &end, 10);
                    if (end != buf && v >= 0 && v <= 16384) {
                        mm = static_cast<int>(v);
                    }
                }
            }

            {
                std::unique_lock<std::mutex> lk(lock_);
                if (tof_stop_) {
                    return;
                }
                if (mm >= 35) {
                    tof_mm_ = mm;
                    tof_ts_ = std::chrono::steady_clock::now();
                    ++tof_seq_;
                } else {
                    // VL53L0X reports ~20mm on invalid/no-return or contact.
                    tof_mm_ = -1;
                }
                tof_cv_.wait_for(lk, std::chrono::milliseconds(120),
                                 [&]() { return tof_stop_; });
                if (tof_stop_) {
                    return;
                }
            }
        }
    }

    void start_scan_locked(bool active) {
        active_scan_ = active;
        phase_ = Phase::kCoarse;
        step_idx_ = 0;
        best_coarse_idx_ = 0;
        best_score_ = -1.0;
        best_pos_ = current_pos_;
        coarse_scores_.clear();
        coarse_steps_.clear();
        scan_used_tof_ = false;
        scan_tof_mm_ = -1;

        int tof_mm = roi_is_near_center_locked() ? fresh_tof_mm_locked() : -1;
        if (tof_mm >= 60 && tof_mm <= 1350) {
            int est = std::clamp(
                    diopters_to_vcm(1000.0f / static_cast<float>(tof_mm)) +
                            tof_vcm_offset_,
                    kVcmMacroPos, kVcmInfPos);
            const int offs[] = {120, 60, 0, -60, -120};
            for (int off : offs) {
                int p = std::clamp(est + off, kVcmMacroPos, kVcmInfPos);
                if (std::find(coarse_steps_.begin(), coarse_steps_.end(), p) ==
                    coarse_steps_.end()) {
                    coarse_steps_.push_back(p);
                }
            }
            scan_used_tof_ = true;
            scan_tof_mm_ = tof_mm;
            ALOGI("AF laser bracket: %dmm -> est=%d (off=%d, %zu steps)",
                  tof_mm, est, tof_vcm_offset_, coarse_steps_.size());
        } else if (tof_mm >= 1400) {
            // Laser reports far/infinity (>1.4m): sweep far-to-mid only.
            coarse_steps_ = {1020, 975, 930, 875};
            scan_used_tof_ = true;
            scan_tof_mm_ = tof_mm;
            ALOGI("AF laser far: %dmm -> far sweep (4 steps)", tof_mm);
        } else {
            // Off-center tap or no valid laser return: full range sweep.
            coarse_steps_ = {1020, 960, 890, 800, 680, 560, 440};
        }

        if (std::abs(current_pos_ - coarse_steps_.back()) <
            std::abs(current_pos_ - coarse_steps_.front())) {
            std::reverse(coarse_steps_.begin(), coarse_steps_.end());
        }
        lens_state_ = ANDROID_LENS_STATE_MOVING;
        af_state_ = active ? ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN
                           : ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN;
        set_vcm_locked(coarse_steps_[0]);
        skip_frames_ = kPipelineSkipFrames;
    }

    std::mutex lock_;
    std::condition_variable vcm_cv_;
    std::thread vcm_thread_;
    bool vcm_stop_ = false;
    bool vcm_moving_ = false;
    int target_pos_ = kVcmDefaultPos;
    int hw_pos_ = kVcmDefaultPos;
    int vcm_fd_ = -1;
    std::condition_variable tof_cv_;
    std::thread tof_thread_;
    std::string tof_path_;
    bool tof_stop_ = false;
    int tof_mm_ = -1;
    std::chrono::steady_clock::time_point tof_ts_{};
    uint64_t tof_seq_ = 0;
    uint64_t last_checked_tof_seq_ = 0;
    int locked_tof_mm_ = -1;
    int tof_change_streak_ = 0;
    bool scan_used_tof_ = false;
    int scan_tof_mm_ = -1;
    int tof_vcm_offset_ = 0;
    int tof_cal_samples_ = 0;
    int32_t active_w_ = 4656;
    int32_t active_h_ = 3496;
    uint8_t af_mode_ = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    uint8_t af_state_ = ANDROID_CONTROL_AF_STATE_INACTIVE;
    uint8_t lens_state_ = ANDROID_LENS_STATE_STATIONARY;
    int current_pos_ = kVcmDefaultPos;
    AfRegionNorm roi_{};

    Phase phase_ = Phase::kIdle;
    bool active_scan_ = false;
    bool initial_scan_done_ = false;
    int skip_frames_ = 0;
    int monitor_counter_ = 0;
    size_t step_idx_ = 0;
    size_t best_coarse_idx_ = 0;
    double best_score_ = -1.0;
    int best_pos_ = kVcmDefaultPos;
    int coarse_best_pos_ = kVcmDefaultPos;
    double coarse_best_score_ = -1.0;
    double locked_sharpness_ = 0.0;
    double locked_luma_ = 0.0;
    std::vector<int> coarse_steps_;
    std::vector<double> coarse_scores_;
    std::vector<int> fine_steps_;
    std::vector<double> fine_scores_;
};

// ---- Wrapped camera3_device_t (AF + fast flush drain) ----

struct InflightRequest {
    uint32_t frame_number = 0;
    bool shutter_sent = false;
    bool result_sent = false;
    bool error_request_sent = false;
    std::vector<camera3_stream_buffer_t> pending_buffers;
};

struct WrappedCameraDevice {
    camera3_device_t pub_dev{};  // MUST be first member
    camera3_device_ops_t pub_ops{};
    camera3_callback_ops_t proxy_cb{};
    const camera3_callback_ops_t* fw_cb = nullptr;
    camera3_device_t* inner = nullptr;
    int ext_id = -1;
    bool has_vcm = false;
    AfController af;

    std::mutex inflight_lock;
    std::condition_variable inflight_cv;
    uint64_t last_shutter_ts = 0;
    std::unordered_map<uint32_t, InflightRequest> inflight;
    std::unordered_set<uint32_t> aborted_frames;
    std::unordered_map<int, camera_metadata_t*> default_templates;
};

WrappedCameraDevice* from_dev(const camera3_device_t* dev) {
    return reinterpret_cast<WrappedCameraDevice*>(
            const_cast<camera3_device_t*>(dev));
}

WrappedCameraDevice* from_cb(const camera3_callback_ops_t* cb) {
    return reinterpret_cast<WrappedCameraDevice*>(
            reinterpret_cast<char*>(const_cast<camera3_callback_ops_t*>(cb)) -
            offsetof(WrappedCameraDevice, proxy_cb));
}

bool is_complete_locked(const InflightRequest& req) {
    return req.pending_buffers.empty() &&
           (req.result_sent || req.error_request_sent) &&
           (req.shutter_sent || req.error_request_sent);
}

uint64_t next_boottime_ns(WrappedCameraDevice* w) {
    struct timespec ts {};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
                   static_cast<uint64_t>(ts.tv_nsec);
    if (now <= w->last_shutter_ts) {
        now = w->last_shutter_ts + 1000ULL;
    }
    w->last_shutter_ts = now;
    return now;
}

void proxy_process_capture_result(const camera3_callback_ops_t* cb,
                                  const camera3_capture_result_t* result) {
    if (!result) {
        return;
    }
    WrappedCameraDevice* w = from_cb(cb);
    const camera3_callback_ops_t* fw_cb = nullptr;
    bool need_synth_shutter = false;
    uint64_t synth_ts = 0;
    bool already_errored = false;

    {
        std::lock_guard<std::mutex> guard(w->inflight_lock);
        if (w->aborted_frames.count(result->frame_number)) {
            return;
        }
        fw_cb = w->fw_cb;
        auto it = w->inflight.find(result->frame_number);
        if (it != w->inflight.end()) {
            already_errored = it->second.error_request_sent;
            if (!already_errored && !it->second.shutter_sent &&
                result->result != nullptr) {
                // libcamera sent result metadata without a valid shutter
                // timestamp; synthesize a monotonic shutter so Camera3Device
                // never stalls 5s in waitUntilDrainedLocked with Timestamp=0.
                it->second.shutter_sent = true;
                need_synth_shutter = true;
                synth_ts = next_boottime_ns(w);
            }
            if (result->result != nullptr) {
                it->second.result_sent = true;
            }
            for (uint32_t i = 0; i < result->num_output_buffers; ++i) {
                const auto& ob = result->output_buffers[i];
                auto& pb = it->second.pending_buffers;
                pb.erase(std::remove_if(pb.begin(), pb.end(),
                                        [&](const camera3_stream_buffer_t& b) {
                                            return b.stream == ob.stream;
                                        }),
                         pb.end());
            }
            if (is_complete_locked(it->second)) {
                w->inflight.erase(it);
                if (w->inflight.empty()) {
                    w->inflight_cv.notify_all();
                }
            }
        }
    }

    if (!fw_cb || !fw_cb->process_capture_result) {
        return;
    }

    if (need_synth_shutter && fw_cb->notify) {
        ALOGW("Camera %d frame %u: synthesizing missing shutter ts=%llu",
              w->ext_id, result->frame_number,
              static_cast<unsigned long long>(synth_ts));
        camera3_notify_msg_t smsg{};
        smsg.type = CAMERA3_MSG_SHUTTER;
        smsg.message.shutter.frame_number = result->frame_number;
        smsg.message.shutter.timestamp = synth_ts;
        fw_cb->notify(fw_cb, &smsg);
    }

    if (already_errored) {
        // After CAMERA3_MSG_ERROR_REQUEST, Camera3 spec forbids sending result
        // metadata or OK buffers; strip result and force BUFFER_STATUS_ERROR.
        camera3_capture_result_t err_res = *result;
        err_res.result = nullptr;
        err_res.partial_result = 0;
        std::vector<camera3_stream_buffer_t> bufs;
        if (result->num_output_buffers > 0 && result->output_buffers) {
            bufs.assign(result->output_buffers,
                        result->output_buffers + result->num_output_buffers);
            for (auto& b : bufs) {
                b.status = CAMERA3_BUFFER_STATUS_ERROR;
            }
            err_res.output_buffers = bufs.data();
        }
        if (err_res.num_output_buffers > 0) {
            fw_cb->process_capture_result(fw_cb, &err_res);
        }
        return;
    }

    if (w->has_vcm) {
        // Feed first completed preview/YUV output buffer to CDAF.
        for (uint32_t i = 0; i < result->num_output_buffers; ++i) {
            const auto& ob = result->output_buffers[i];
            if (ob.status == CAMERA3_BUFFER_STATUS_OK && ob.buffer &&
                *ob.buffer && ob.stream &&
                ob.stream->format != HAL_PIXEL_FORMAT_BLOB) {
                w->af.on_preview_frame(*ob.buffer);
                break;
            }
        }

        if (result->result != nullptr) {
            camera_metadata_t* patched =
                    copy_and_grow_metadata(result->result, 8, 128);
            if (patched) {
                uint8_t af_mode = 0, af_state = 0, lens_state = 0;
                float focus_dist = 0.0f;
                AfRegionNorm roi{};
                w->af.get_result_state(&af_mode, &af_state, &lens_state,
                                       &focus_dist, &roi);
                set_or_add_entry(patched, ANDROID_CONTROL_AF_MODE, &af_mode, 1);
                set_or_add_entry(patched, ANDROID_CONTROL_AF_STATE, &af_state,
                                 1);
                set_or_add_entry(patched, ANDROID_LENS_STATE, &lens_state, 1);
                set_or_add_entry(patched, ANDROID_LENS_FOCUS_DISTANCE,
                                 &focus_dist, 1);
                if (roi.has_raw) {
                    set_or_add_entry(patched, ANDROID_CONTROL_AF_REGIONS,
                                     roi.raw, 5);
                }
                sort_camera_metadata(patched);

                camera3_capture_result_t mod = *result;
                mod.result = patched;
                fw_cb->process_capture_result(fw_cb, &mod);
                free_camera_metadata(patched);
                return;
            }
        }
    }

    fw_cb->process_capture_result(fw_cb, result);
}

void proxy_notify(const camera3_callback_ops_t* cb,
                  const camera3_notify_msg_t* msg) {
    if (!msg) {
        return;
    }
    WrappedCameraDevice* w = from_cb(cb);
    const camera3_callback_ops_t* fw_cb = nullptr;

    camera3_notify_msg_t patched_msg = *msg;
    const camera3_notify_msg_t* out_msg = msg;

    {
        std::lock_guard<std::mutex> guard(w->inflight_lock);
        uint32_t frame = 0;
        bool has_frame = false;
        if (msg->type == CAMERA3_MSG_SHUTTER) {
            frame = msg->message.shutter.frame_number;
            has_frame = true;
            if (msg->message.shutter.timestamp == 0) {
                patched_msg.message.shutter.timestamp = next_boottime_ns(w);
                out_msg = &patched_msg;
                ALOGW("Camera %d frame %u: replaced 0 shutter ts with %llu",
                      w->ext_id, frame,
                      static_cast<unsigned long long>(
                              patched_msg.message.shutter.timestamp));
            } else if (msg->message.shutter.timestamp > w->last_shutter_ts) {
                w->last_shutter_ts = msg->message.shutter.timestamp;
            }
        } else if (msg->type == CAMERA3_MSG_ERROR) {
            frame = msg->message.error.frame_number;
            has_frame = true;
        }
        if (has_frame && w->aborted_frames.count(frame)) {
            return;
        }
        fw_cb = w->fw_cb;
        if (has_frame) {
            auto it = w->inflight.find(frame);
            if (it != w->inflight.end()) {
                if (msg->type == CAMERA3_MSG_SHUTTER) {
                    it->second.shutter_sent = true;
                } else if (msg->type == CAMERA3_MSG_ERROR) {
                    if (msg->message.error.error_code ==
                        CAMERA3_MSG_ERROR_REQUEST) {
                        it->second.error_request_sent = true;
                    } else if (msg->message.error.error_code ==
                               CAMERA3_MSG_ERROR_RESULT) {
                        it->second.result_sent = true;
                    } else if (msg->message.error.error_code ==
                                       CAMERA3_MSG_ERROR_BUFFER &&
                               msg->message.error.error_stream) {
                        auto& pb = it->second.pending_buffers;
                        pb.erase(std::remove_if(
                                         pb.begin(), pb.end(),
                                         [&](const camera3_stream_buffer_t& b) {
                                             return b.stream ==
                                                    msg->message.error
                                                            .error_stream;
                                         }),
                                 pb.end());
                    }
                }
                if (is_complete_locked(it->second)) {
                    w->inflight.erase(it);
                    if (w->inflight.empty()) {
                        w->inflight_cv.notify_all();
                    }
                }
            }
        }
    }

    if (fw_cb && fw_cb->notify) {
        fw_cb->notify(fw_cb, out_msg);
    }
}

int dev_initialize(const camera3_device_t* dev,
                   const camera3_callback_ops_t* callback_ops) {
    WrappedCameraDevice* w = from_dev(dev);
    {
        std::lock_guard<std::mutex> guard(w->inflight_lock);
        w->fw_cb = callback_ops;
    }
    return w->inner->ops->initialize(w->inner, &w->proxy_cb);
}

int dev_configure_streams(const camera3_device_t* dev,
                          camera3_stream_configuration_t* stream_list) {
    WrappedCameraDevice* w = from_dev(dev);
    {
        std::lock_guard<std::mutex> guard(w->inflight_lock);
        w->inflight.clear();
        w->aborted_frames.clear();
    }
    return w->inner->ops->configure_streams(w->inner, stream_list);
}

const camera_metadata_t* dev_construct_default_request_settings(
        const camera3_device_t* dev, int type) {
    WrappedCameraDevice* w = from_dev(dev);
    const camera_metadata_t* base =
            w->inner->ops->construct_default_request_settings(w->inner, type);
    if (!base || !w->has_vcm) {
        return base;
    }

    std::lock_guard<std::mutex> guard(w->inflight_lock);
    auto it = w->default_templates.find(type);
    if (it != w->default_templates.end()) {
        return it->second;
    }

    camera_metadata_t* copy = copy_and_grow_metadata(base, 4, 64);
    if (!copy) {
        return base;
    }
    uint8_t af_mode = (type == CAMERA3_TEMPLATE_VIDEO_RECORD ||
                       type == CAMERA3_TEMPLATE_VIDEO_SNAPSHOT)
                              ? ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO
                              : ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    if (type == CAMERA3_TEMPLATE_MANUAL) {
        af_mode = ANDROID_CONTROL_AF_MODE_OFF;
    }
    float dist = kHyperfocalDiopters;
    set_or_add_entry(copy, ANDROID_CONTROL_AF_MODE, &af_mode, 1);
    set_or_add_entry(copy, ANDROID_LENS_FOCUS_DISTANCE, &dist, 1);
    sort_camera_metadata(copy);
    w->default_templates[type] = copy;
    return copy;
}

int dev_process_capture_request(const camera3_device_t* dev,
                                camera3_capture_request_t* request) {
    WrappedCameraDevice* w = from_dev(dev);
    if (!request) {
        return -EINVAL;
    }

    if (w->has_vcm) {
        w->af.on_request(request->settings);
    }

    {
        std::lock_guard<std::mutex> guard(w->inflight_lock);
        InflightRequest ir{};
        ir.frame_number = request->frame_number;
        ir.pending_buffers.reserve(request->num_output_buffers);
        for (uint32_t i = 0; i < request->num_output_buffers; ++i) {
            camera3_stream_buffer_t sb = request->output_buffers[i];
            sb.acquire_fence = -1;
            sb.release_fence = -1;
            sb.status = CAMERA3_BUFFER_STATUS_ERROR;
            ir.pending_buffers.push_back(sb);
        }
        w->inflight[request->frame_number] = std::move(ir);
    }

    int rc = w->inner->ops->process_capture_request(w->inner, request);
    if (rc != 0) {
        std::lock_guard<std::mutex> guard(w->inflight_lock);
        w->inflight.erase(request->frame_number);
    }
    return rc;
}

void dev_dump(const camera3_device_t* dev, int fd) {
    WrappedCameraDevice* w = from_dev(dev);
    if (w->inner->ops->dump) {
        w->inner->ops->dump(w->inner, fd);
    }
}

void drain_leftover_inflight(WrappedCameraDevice* w, const char* caller) {
    std::vector<InflightRequest> leftovers;
    const camera3_callback_ops_t* fw_cb = nullptr;
    {
        std::lock_guard<std::mutex> guard(w->inflight_lock);
        fw_cb = w->fw_cb;
        if (!w->inflight.empty()) {
            leftovers.reserve(w->inflight.size());
            for (auto& kv : w->inflight) {
                w->aborted_frames.insert(kv.first);
                leftovers.push_back(std::move(kv.second));
            }
            w->inflight.clear();
        }
    }

    if (leftovers.empty() || !fw_cb) {
        return;
    }

    ALOGW("Camera %d %s: draining %zu leftover in-flight request(s)",
          w->ext_id, caller, leftovers.size());
    for (auto& ir : leftovers) {
        ALOGW("  leftover frame=%u shutter=%d result=%d err=%d bufs=%zu",
              ir.frame_number, ir.shutter_sent ? 1 : 0,
              ir.result_sent ? 1 : 0, ir.error_request_sent ? 1 : 0,
              ir.pending_buffers.size());
        if (!ir.shutter_sent && !ir.error_request_sent && fw_cb->notify) {
            camera3_notify_msg_t msg{};
            msg.type = CAMERA3_MSG_ERROR;
            msg.message.error.frame_number = ir.frame_number;
            msg.message.error.error_stream = nullptr;
            msg.message.error.error_code = CAMERA3_MSG_ERROR_REQUEST;
            fw_cb->notify(fw_cb, &msg);
        } else if (ir.shutter_sent && !ir.result_sent &&
                   !ir.error_request_sent && fw_cb->notify) {
            camera3_notify_msg_t msg{};
            msg.type = CAMERA3_MSG_ERROR;
            msg.message.error.frame_number = ir.frame_number;
            msg.message.error.error_stream = nullptr;
            msg.message.error.error_code = CAMERA3_MSG_ERROR_RESULT;
            fw_cb->notify(fw_cb, &msg);
        }
        if (!ir.pending_buffers.empty() && fw_cb->process_capture_result) {
            camera3_capture_result_t res{};
            res.frame_number = ir.frame_number;
            res.result = nullptr;
            res.num_output_buffers =
                    static_cast<uint32_t>(ir.pending_buffers.size());
            res.output_buffers = ir.pending_buffers.data();
            res.partial_result = 0;
            fw_cb->process_capture_result(fw_cb, &res);
        }
    }
}

int dev_flush(const camera3_device_t* dev) {
    WrappedCameraDevice* w = from_dev(dev);
    auto t0 = std::chrono::steady_clock::now();
    int rc = w->inner->ops->flush ? w->inner->ops->flush(w->inner) : 0;
    // Wait briefly for libcamera's async PostProcessorWorker to finish any
    // in-progress frame conversion before force-draining, avoiding use-after-
    // unmap SIGSEGV while still staying far below Camera3's 5s drain timeout.
    {
        std::unique_lock<std::mutex> lk(w->inflight_lock);
        w->inflight_cv.wait_for(lk, std::chrono::milliseconds(350),
                                [&]() { return w->inflight.empty(); });
    }
    drain_leftover_inflight(w, "flush");
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
    ALOGI("Camera %d flush() completed in %lld ms (rc=%d)", w->ext_id,
          static_cast<long long>(ms), rc);
    return rc;
}

void dev_signal_stream_flush(const camera3_device_t* dev, uint32_t num_streams,
                             const camera3_stream_t* const* streams) {
    WrappedCameraDevice* w = from_dev(dev);
    if (w->inner->ops->signal_stream_flush) {
        w->inner->ops->signal_stream_flush(w->inner, num_streams, streams);
    }
}

int dev_close(hw_device_t* device) {
    WrappedCameraDevice* w = reinterpret_cast<WrappedCameraDevice*>(device);
    drain_leftover_inflight(w, "close");
    if (w->has_vcm) {
        w->af.close();
    }
    int rc = 0;
    if (w->inner && w->inner->common.close) {
        rc = w->inner->common.close(&w->inner->common);
    }
    for (auto& kv : w->default_templates) {
        free_camera_metadata(kv.second);
    }
    delete w;
    return rc;
}

// ---- Module loading & camera ID mapping ----

int ensure_loaded_locked() {
    if (g_libcam != nullptr) {
        return 0;
    }

    g_dso = dlopen(kLibcameraHalPath, RTLD_NOW | RTLD_LOCAL);
    if (!g_dso) {
        ALOGE("dlopen(%s) failed: %s", kLibcameraHalPath, dlerror());
        return -ENODEV;
    }

    auto* sym = reinterpret_cast<camera_module_t*>(
            dlsym(g_dso, HAL_MODULE_INFO_SYM_AS_STR));
    if (!sym) {
        ALOGE("dlsym(%s) failed: %s", HAL_MODULE_INFO_SYM_AS_STR, dlerror());
        dlclose(g_dso);
        g_dso = nullptr;
        return -ENODEV;
    }

    g_libcam = sym;
    g_set_torch_strength = reinterpret_cast<SetTorchStrengthFn>(
            dlsym(g_dso, "camera_module_set_torch_strength"));
    g_get_torch_strength = reinterpret_cast<GetTorchStrengthFn>(
            dlsym(g_dso, "camera_module_get_torch_strength"));
    g_vcm_dev_path = find_bu24235_subdev();
    if (!g_vcm_dev_path.empty()) {
        ALOGI("Found BU24235 VCM subdev: %s", g_vcm_dev_path.c_str());
    }
    g_tof_sysfs_path = find_vl53l0x_sysfs();
    if (!g_tof_sysfs_path.empty()) {
        ALOGI("Found VL53L0X laser AF: %s", g_tof_sysfs_path.c_str());
    }

    if (g_libcam->init) {
        int rc = g_libcam->init();
        if (rc != 0) {
            ALOGE("libcamera init() failed: %d", rc);
            return rc;
        }
    }
    return 0;
}

void build_map_locked() {
    if (g_mapped || !g_libcam) {
        return;
    }

    const int n = g_libcam->get_number_of_cameras
                          ? g_libcam->get_number_of_cameras()
                          : 0;
    g_cameras.clear();
    g_cameras.reserve(n > 0 ? n : 0);

    for (int raw_id = 0; raw_id < n; ++raw_id) {
        camera_info raw_info{};
        if (g_libcam->get_camera_info(raw_id, &raw_info) != 0) {
            continue;
        }

        int32_t array_w = 0;
        int32_t array_h = 0;
        if (raw_info.static_camera_characteristics) {
            camera_metadata_ro_entry_t entry{};
            if (find_camera_metadata_ro_entry(
                        raw_info.static_camera_characteristics,
                        ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, &entry) == 0 &&
                entry.count >= 2) {
                array_w = entry.data.i32[0];
                array_h = entry.data.i32[1];
            }
        }

        CameraEntry e{};
        e.raw_id = raw_id;
        e.info = raw_info;
        e.active_w = array_w;
        e.active_h = array_h;

        if (raw_info.facing == CAMERA_FACING_BACK && array_w >= 4400) {
            // Main 16 MP Sony IMX351 (71 deg FOV, 4.03 mm, f/1.6, BU24235 AF)
            e.priority = 0;
            e.focal_length = 4.03f;
            e.aperture = 1.6f;
            e.has_vcm = !g_vcm_dev_path.empty();
        } else if (raw_info.facing == CAMERA_FACING_FRONT) {
            // Front 5 MP Hynix HI553 (2.46 mm, f/2.2, fixed focus)
            e.priority = 1;
            e.focal_length = 2.46f;
            e.aperture = 2.2f;
        } else if (raw_info.facing == CAMERA_FACING_BACK) {
            // Ultrawide 13 MP Samsung S5K3M3 (120 deg FOV, 1.52 mm, f/1.9)
            e.priority = 2;
            e.focal_length = 1.52f;
            e.aperture = 1.9f;
        } else {
            e.priority = 10 + raw_id;
        }

        if (raw_info.static_camera_characteristics) {
            e.patched_meta = copy_and_grow_metadata(
                    raw_info.static_camera_characteristics, 16, 256);
            if (e.patched_meta) {
                if (e.focal_length > 0.0f) {
                    set_or_add_entry(e.patched_meta,
                                     ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
                                     &e.focal_length, 1);
                }
                if (e.aperture > 0.0f) {
                    set_or_add_entry(e.patched_meta,
                                     ANDROID_LENS_INFO_AVAILABLE_APERTURES,
                                     &e.aperture, 1);
                }
                if (e.has_vcm) {
                    const uint8_t af_modes[] = {
                        ANDROID_CONTROL_AF_MODE_OFF,
                        ANDROID_CONTROL_AF_MODE_AUTO,
                        ANDROID_CONTROL_AF_MODE_MACRO,
                        ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO,
                        ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE,
                    };
                    set_or_add_entry(e.patched_meta,
                                     ANDROID_CONTROL_AF_AVAILABLE_MODES,
                                     af_modes, sizeof(af_modes));
                    const int32_t max_regions[3] = {0, 0, 1};
                    set_or_add_entry(e.patched_meta,
                                     ANDROID_CONTROL_MAX_REGIONS, max_regions,
                                     3);
                    const float min_focus = kMinFocusDiopters;
                    const float hyperfocal = kHyperfocalDiopters;
                    const uint8_t cal =
                            ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE;
                    set_or_add_entry(e.patched_meta,
                                     ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
                                     &min_focus, 1);
                    set_or_add_entry(e.patched_meta,
                                     ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
                                     &hyperfocal, 1);
                    set_or_add_entry(
                            e.patched_meta,
                            ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &cal,
                            1);
                    append_unique_i32_keys(
                            e.patched_meta,
                            ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
                            {ANDROID_LENS_FOCUS_DISTANCE,
                             ANDROID_CONTROL_AF_REGIONS});
                    append_unique_i32_keys(
                            e.patched_meta,
                            ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
                            {ANDROID_LENS_FOCUS_DISTANCE, ANDROID_LENS_STATE,
                             ANDROID_CONTROL_AF_REGIONS});
                    append_unique_i32_keys(
                            e.patched_meta,
                            ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                            {ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION});
                }
                sort_camera_metadata(e.patched_meta);
                e.info.static_camera_characteristics = e.patched_meta;
            }
        }

        g_cameras.push_back(e);
    }

    std::stable_sort(g_cameras.begin(), g_cameras.end(),
                     [](const CameraEntry& a, const CameraEntry& b) {
                         return a.priority < b.priority;
                     });

    for (size_t ext = 0; ext < g_cameras.size(); ++ext) {
        ALOGI("Mapped ext camera %zu -> libcamera %d (facing=%d, f=%.2fmm, "
              "f/%.1f, vcm=%d)",
              ext, g_cameras[ext].raw_id, g_cameras[ext].info.facing,
              g_cameras[ext].focal_length, g_cameras[ext].aperture,
              g_cameras[ext].has_vcm ? 1 : 0);
    }

    g_mapped = true;
}

int ext_to_raw_locked(int ext_id) {
    if (ext_id < 0 || static_cast<size_t>(ext_id) >= g_cameras.size()) {
        return -1;
    }
    return g_cameras[ext_id].raw_id;
}

int raw_to_ext_locked(int raw_id) {
    for (size_t i = 0; i < g_cameras.size(); ++i) {
        if (g_cameras[i].raw_id == raw_id) {
            return static_cast<int>(i);
        }
    }
    return raw_id;
}

bool parse_id(const char* str, int* out) {
    if (!str || *str == '\0') {
        return false;
    }
    char* end = nullptr;
    long v = strtol(str, &end, 10);
    if (end == str || *end != '\0' || v < 0 || v > 1024) {
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

void proxy_camera_device_status_change(const camera_module_callbacks_t*,
                                       int raw_camera_id, int new_status) {
    const camera_module_callbacks_t* cb = nullptr;
    int ext_id = raw_camera_id;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        cb = g_fw_callbacks;
        if (g_mapped) {
            ext_id = raw_to_ext_locked(raw_camera_id);
        }
    }
    if (cb && cb->camera_device_status_change) {
        cb->camera_device_status_change(cb, ext_id, new_status);
    }
}

void proxy_torch_mode_status_change(const camera_module_callbacks_t*,
                                    const char* raw_camera_id, int new_status) {
    const camera_module_callbacks_t* cb = nullptr;
    char ext_buf[16];
    const char* ext_str = raw_camera_id;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        cb = g_fw_callbacks;
        int raw_id = -1;
        if (g_mapped && parse_id(raw_camera_id, &raw_id)) {
            int ext_id = raw_to_ext_locked(raw_id);
            snprintf(ext_buf, sizeof(ext_buf), "%d", ext_id);
            ext_str = ext_buf;
        }
    }
    if (cb && cb->torch_mode_status_change) {
        cb->torch_mode_status_change(cb, ext_str, new_status);
    }
}

const camera_module_callbacks_t g_proxy_callbacks = {
    .camera_device_status_change = proxy_camera_device_status_change,
    .torch_mode_status_change = proxy_torch_mode_status_change,
};

int wrapper_open(const hw_module_t* module, const char* id,
                 hw_device_t** device) {
    if (!device) {
        return -EINVAL;
    }
    camera_module_t* libcam = nullptr;
    char raw_buf[16];
    int ext_id = -1;
    bool has_vcm = false;
    int32_t active_w = 0, active_h = 0;
    std::string vcm_path;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        int rc = ensure_loaded_locked();
        if (rc != 0) {
            return rc;
        }
        build_map_locked();
        if (!parse_id(id, &ext_id)) {
            return -EINVAL;
        }
        int raw_id = ext_to_raw_locked(ext_id);
        if (raw_id < 0) {
            return -EINVAL;
        }
        snprintf(raw_buf, sizeof(raw_buf), "%d", raw_id);
        libcam = g_libcam;
        has_vcm = g_cameras[ext_id].has_vcm;
        active_w = g_cameras[ext_id].active_w;
        active_h = g_cameras[ext_id].active_h;
        vcm_path = g_vcm_dev_path;
    }

    hw_device_t* inner_hw = nullptr;
    int rc = libcam->common.methods->open(&libcam->common, raw_buf, &inner_hw);
    if (rc != 0 || !inner_hw) {
        return rc != 0 ? rc : -ENODEV;
    }

    auto* inner = reinterpret_cast<camera3_device_t*>(inner_hw);
    auto* w = new WrappedCameraDevice();
    w->inner = inner;
    w->ext_id = ext_id;
    w->pub_dev.common = inner->common;
    w->pub_dev.common.module = const_cast<hw_module_t*>(module);
    w->pub_dev.common.close = dev_close;
    w->pub_ops = *inner->ops;
    w->pub_ops.initialize = dev_initialize;
    w->pub_ops.configure_streams = dev_configure_streams;
    w->pub_ops.construct_default_request_settings =
            dev_construct_default_request_settings;
    w->pub_ops.process_capture_request = dev_process_capture_request;
    w->pub_ops.dump = dev_dump;
    w->pub_ops.flush = dev_flush;
    if (inner->ops->signal_stream_flush) {
        w->pub_ops.signal_stream_flush = dev_signal_stream_flush;
    }
    w->pub_dev.ops = &w->pub_ops;
    w->pub_dev.priv = w;
    w->proxy_cb.process_capture_result = proxy_process_capture_result;
    w->proxy_cb.notify = proxy_notify;

    if (has_vcm && !vcm_path.empty()) {
        int vcm_fd = open(vcm_path.c_str(), O_RDWR | O_CLOEXEC);
        if (vcm_fd >= 0) {
            std::string tof_path;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                tof_path = g_tof_sysfs_path;
            }
            w->has_vcm = true;
            w->af.init(vcm_fd, active_w, active_h, tof_path);
            ALOGI("Camera %d opened VCM %s (fd=%d, tof=%s)", ext_id,
                  vcm_path.c_str(), vcm_fd,
                  tof_path.empty() ? "none" : tof_path.c_str());
        } else {
            ALOGW("Camera %d failed to open VCM %s: %s", ext_id,
                  vcm_path.c_str(), strerror(errno));
        }
    }

    *device = &w->pub_dev.common;
    return 0;
}

int wrapper_get_number_of_cameras() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (ensure_loaded_locked() != 0) {
        return 0;
    }
    build_map_locked();
    return static_cast<int>(g_cameras.size());
}

int wrapper_get_camera_info(int camera_id, camera_info* info) {
    if (!info) {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> guard(g_lock);
    int rc = ensure_loaded_locked();
    if (rc != 0) {
        return rc;
    }
    build_map_locked();
    if (camera_id < 0 || static_cast<size_t>(camera_id) >= g_cameras.size()) {
        return -EINVAL;
    }
    *info = g_cameras[camera_id].info;
    return 0;
}

int wrapper_set_callbacks(const camera_module_callbacks_t* callbacks) {
    camera_module_t* libcam = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        int rc = ensure_loaded_locked();
        if (rc != 0) {
            return rc;
        }
        build_map_locked();
        g_fw_callbacks = callbacks;
        libcam = g_libcam;
    }
    if (!libcam->set_callbacks) {
        return 0;
    }
    return libcam->set_callbacks(callbacks ? &g_proxy_callbacks : nullptr);
}

void wrapper_get_vendor_tag_ops(vendor_tag_ops_t* ops) {
    camera_module_t* libcam = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (ensure_loaded_locked() != 0) {
            return;
        }
        libcam = g_libcam;
    }
    if (libcam && libcam->get_vendor_tag_ops) {
        libcam->get_vendor_tag_ops(ops);
    }
}

int wrapper_set_torch_mode(const char* camera_id, bool enabled) {
    camera_module_t* libcam = nullptr;
    char raw_buf[16];
    {
        std::lock_guard<std::mutex> guard(g_lock);
        int rc = ensure_loaded_locked();
        if (rc != 0) {
            return rc;
        }
        build_map_locked();
        int ext_id = -1;
        if (!parse_id(camera_id, &ext_id)) {
            return -EINVAL;
        }
        int raw_id = ext_to_raw_locked(ext_id);
        if (raw_id < 0) {
            return -EINVAL;
        }
        snprintf(raw_buf, sizeof(raw_buf), "%d", raw_id);
        libcam = g_libcam;
    }
    if (!libcam->set_torch_mode) {
        return -ENOSYS;
    }
    return libcam->set_torch_mode(raw_buf, enabled);
}

int wrapper_init() {
    std::lock_guard<std::mutex> guard(g_lock);
    int rc = ensure_loaded_locked();
    if (rc != 0) {
        return rc;
    }
    build_map_locked();
    return 0;
}

hw_module_methods_t g_module_methods = {
    .open = wrapper_open,
};

}  // namespace

extern "C" {

__attribute__((visibility("default")))
int camera_module_set_torch_strength(const char* camera_id, int strength) {
    SetTorchStrengthFn fn = nullptr;
    char raw_buf[16];
    {
        std::lock_guard<std::mutex> guard(g_lock);
        int rc = ensure_loaded_locked();
        if (rc != 0) {
            return rc;
        }
        build_map_locked();
        int ext_id = -1;
        if (!parse_id(camera_id, &ext_id)) {
            return -EINVAL;
        }
        int raw_id = ext_to_raw_locked(ext_id);
        if (raw_id < 0) {
            return -EINVAL;
        }
        snprintf(raw_buf, sizeof(raw_buf), "%d", raw_id);
        fn = g_set_torch_strength;
    }
    return fn ? fn(raw_buf, strength) : -ENOSYS;
}

__attribute__((visibility("default")))
int camera_module_get_torch_strength(const char* camera_id, int* strength) {
    GetTorchStrengthFn fn = nullptr;
    char raw_buf[16];
    {
        std::lock_guard<std::mutex> guard(g_lock);
        int rc = ensure_loaded_locked();
        if (rc != 0) {
            return rc;
        }
        build_map_locked();
        int ext_id = -1;
        if (!parse_id(camera_id, &ext_id)) {
            return -EINVAL;
        }
        int raw_id = ext_to_raw_locked(ext_id);
        if (raw_id < 0) {
            return -EINVAL;
        }
        snprintf(raw_buf, sizeof(raw_buf), "%d", raw_id);
        fn = g_get_torch_strength;
    }
    return fn ? fn(raw_buf, strength) : -ENOSYS;
}

__attribute__((visibility("default")))
camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CAMERA_HARDWARE_MODULE_ID,
        .name = "LG V30 libcamera HAL wrapper",
        .author = "LineageOS",
        .methods = &g_module_methods,
        .dso = nullptr,
        .reserved = {0},
    },
    .get_number_of_cameras = wrapper_get_number_of_cameras,
    .get_camera_info = wrapper_get_camera_info,
    .set_callbacks = wrapper_set_callbacks,
    .get_vendor_tag_ops = wrapper_get_vendor_tag_ops,
    .open_legacy = nullptr,
    .set_torch_mode = wrapper_set_torch_mode,
    .init = wrapper_init,
    .get_physical_camera_info = nullptr,
    .is_stream_combination_supported = nullptr,
    .notify_device_state_change = nullptr,
    .reserved = {nullptr, nullptr},
};

}  // extern "C"
