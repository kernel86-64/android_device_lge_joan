// SPDX-License-Identifier: Apache-2.0
// Legacy msm8998 gralloc1 handle ABI from hardware/qcom/display/gralloc/gr_priv_handle.h.
#undef LOG_TAG
#define LOG_TAG "drmhwc-joan"
#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "utils/log.h"

#include <cstdint>
#include <limits>

namespace android::drm_hwcomposer {
namespace {
constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// HWC3 treats std::nullopt from GetBoInfo() as kBadLayer, breaking the entire
// display frame. Return a client-only BufferInfo (fb import fails once per
// slot and caches fb=nullptr) so GenericCompositionPlanner falls back to GPU
// CLIENT composition for YUV, secure, or non-scanoutable UBWC layers.
BufferInfo MakeClientFallback(uint32_t width, uint32_t height) {
  BufferInfo info{};
  info.width = width;
  info.height = height;
  info.format = DRM_FORMAT_INVALID;
  info.prime_fds[0] = -1;
  info.modifiers[0] = DRM_FORMAT_MOD_INVALID;
  return info;
}
}  // namespace

class BufferInfoQcom final : public LegacyBufferInfoGetter {
 public:
  auto GetBoInfo(buffer_handle_t handle) -> std::optional<BufferInfo> override {
    // This ABI is the 64-bit msm8998 handle: two dma-buf fds and 24 integers.
    // Do not interpret other Qualcomm generations using these offsets.
    if (handle == nullptr || handle->version != sizeof(native_handle_t) ||
        handle->numFds != 2 || handle->numInts != 24 ||
        handle->data[2] != 0x676d736d) {
      return {};
    }
    const int *data = handle->data;
    if (data[0] < 0 || data[4] <= 0 || data[5] <= 0 || data[6] <= 0 ||
        data[7] <= 0 || data[6] > data[4] || data[7] > data[5] ||
        data[10] <= 0 || data[11] < 0) {
      return {};
    }
    const uint64_t width = static_cast<uint32_t>(data[6]);
    const uint64_t height = static_cast<uint32_t>(data[7]);
    constexpr uint32_t kFlagUbwcAligned = 0x08000000u;
    constexpr uint32_t kUnsupported = 0x00000400u | 0x00004000u |
                                      0x01000000u | 0x02000000u;
    const uint32_t flags = static_cast<uint32_t>(data[3]);
    if ((flags & kUnsupported) != 0) {
      // Secure and non-UBWC tiled layouts need separate KMS support.
      return MakeClientFallback(width, height);
    }
    const bool is_ubwc = (flags & kFlagUbwcAligned) != 0;
    uint32_t bpp = 0;
    uint32_t drm_format = 0;
    if (is_ubwc) {
      // DPU1 UBWC 1.0 natively supports ABGR8888/XBGR8888 (RGBA_8888/RGBX_8888).
      // Fall back to GPU client composition for other UBWC formats (e.g.
      // RGBA_1010102, BGRA_8888, YUV).
      switch (data[8]) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
          drm_format = DRM_FORMAT_ABGR8888;
          bpp = 4;
          break;
        case HAL_PIXEL_FORMAT_RGBX_8888:
          drm_format = DRM_FORMAT_XBGR8888;
          bpp = 4;
          break;
        default:
          return MakeClientFallback(width, height);
      }
    } else {
      switch (data[8]) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_RGBA_1010102:
          bpp = 4;
          break;
        case HAL_PIXEL_FORMAT_RGB_888:
          bpp = 3;
          break;
        case HAL_PIXEL_FORMAT_RGB_565:
          bpp = 2;
          break;
        default:
          // Client composition handles YUV/NV12/camera buffers cleanly.
          return MakeClientFallback(width, height);
      }
      drm_format = (data[8] == HAL_PIXEL_FORMAT_RGB_565)
                       ? DRM_FORMAT_RGB565
                       : ConvertHalFormatToDrm(static_cast<uint32_t>(data[8]));
    }
    const uint64_t pitch = static_cast<uint64_t>(data[4]) * bpp;
    if (pitch > std::numeric_limits<uint32_t>::max()) {
      return MakeClientFallback(width, height);
    }
    if (is_ubwc) {
      // Match drivers/gpu/drm/msm/disp/dpu1/dpu_formats.c UBWC 1.0 RGB layout.
      const uint64_t expected_pitch = AlignUp(width * bpp, 256);
      const uint64_t data_size =
          AlignUp(expected_pitch * AlignUp(height, 16), 4096);
      const uint64_t meta_pitch = AlignUp((width + 15) / 16, 64);
      const uint64_t meta_size =
          AlignUp(meta_pitch * AlignUp((height + 3) / 4, 16), 4096);
      if (data[11] != 0 || pitch != expected_pitch ||
          data_size + meta_size > static_cast<uint64_t>(data[10])) {
        ALOGW("Rejecting UBWC buffer %ux%u pitch=%llu expected=%llu size=%d "
              "need=%llu offset=%d",
              static_cast<uint32_t>(width), static_cast<uint32_t>(height),
              static_cast<unsigned long long>(pitch),
              static_cast<unsigned long long>(expected_pitch), data[10],
              static_cast<unsigned long long>(data_size + meta_size), data[11]);
        return MakeClientFallback(width, height);
      }
    } else {
      const uint64_t end = static_cast<uint64_t>(data[11]) + pitch * height;
      if (end > static_cast<uint32_t>(data[10])) {
        return MakeClientFallback(width, height);
      }
    }
    BufferInfo info{};
    info.width = static_cast<uint32_t>(width);
    info.height = static_cast<uint32_t>(height);
    info.format = drm_format;
    info.pitches[0] = static_cast<uint32_t>(pitch);
    info.offsets[0] = static_cast<uint32_t>(data[11]);
    info.prime_fds[0] = data[0];
    info.modifiers[0] =
        is_ubwc ? DRM_FORMAT_MOD_QCOM_COMPRESSED : DRM_FORMAT_MOD_LINEAR;
    return info;
  }
};
LEGACY_BUFFER_INFO_GETTER(BufferInfoQcom);
}  // namespace android::drm_hwcomposer
