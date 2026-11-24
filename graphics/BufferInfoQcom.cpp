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
    constexpr uint32_t kUnsupported = 0x00000400 | 0x00004000 |
                                     0x01000000 | 0x02000000 | 0x08000000;
    if ((static_cast<uint32_t>(handle->data[3]) & kUnsupported) != 0) {
      // Secure and compressed/tiled layouts need separate KMS support.
      return {};
    }
    const int *data = handle->data;
    if (data[0] < 0 || data[4] <= 0 || data[5] <= 0 || data[6] <= 0 ||
        data[7] <= 0 || data[6] > data[4] || data[7] > data[5] ||
        data[10] <= 0 || data[11] < 0) {
      return {};
    }
    uint32_t bpp = 0;
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
        // Client composition handles YUV until explicit plane layouts exist.
        return {};
    }
    const uint64_t pitch = static_cast<uint64_t>(data[4]) * bpp;
    const uint64_t end = static_cast<uint64_t>(data[11]) +
                         pitch * static_cast<uint32_t>(data[7]);
    if (pitch > std::numeric_limits<uint32_t>::max() ||
        end > static_cast<uint32_t>(data[10])) {
      return {};
    }
    BufferInfo info{};
    info.width = static_cast<uint32_t>(data[6]);
    info.height = static_cast<uint32_t>(data[7]);
    info.format = (data[8] == HAL_PIXEL_FORMAT_RGB_565)
                      ? DRM_FORMAT_RGB565
                      : ConvertHalFormatToDrm(static_cast<uint32_t>(data[8]));
    info.pitches[0] = static_cast<uint32_t>(pitch);
    info.offsets[0] = static_cast<uint32_t>(data[11]);
    info.prime_fds[0] = data[0];
    info.modifiers[0] = DRM_FORMAT_MOD_LINEAR;
    return info;
  }
};
LEGACY_BUFFER_INFO_GETTER(BufferInfoQcom);
}  // namespace android::drm_hwcomposer
