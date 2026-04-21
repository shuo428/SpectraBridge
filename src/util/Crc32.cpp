#include "util/Crc32.h"

namespace spectra {
namespace util {

uint32_t ComputeCrc32(const uint8_t* data, std::size_t size)
{
    // 初值和最终取反遵循常见 CRC32 实现，便于与 FPGA 侧对齐。
    uint32_t crc = 0xFFFFFFFFu;

    for (std::size_t index = 0u; index < size; ++index)
    {
        crc ^= static_cast<uint32_t>(data[index]);

        // 按位推进 8 次，把当前字节完整混入 CRC。
        for (int bit = 0; bit < 8; ++bit)
        {
            const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

}  // namespace util
}  // namespace spectra
