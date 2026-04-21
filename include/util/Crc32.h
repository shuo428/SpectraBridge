#ifndef SPECTRA_BRIDGE_UTIL_CRC32_H
#define SPECTRA_BRIDGE_UTIL_CRC32_H

#include <cstddef>
#include <cstdint>

namespace spectra {
namespace util {

// 计算标准 CRC32（多项式 0xEDB88320）。
// 用于校验图像 payload 是否在传输过程中损坏。
uint32_t ComputeCrc32(const uint8_t* data, std::size_t size);

}  // namespace util
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_UTIL_CRC32_H
