#include "image/ImageConverter.h"

#include <cstddef>

#include "network/ByteUtils.h"
#include "protocol/ImageProtocol.h"

namespace spectra {
namespace image {

bool ConvertRaw16Low12ToGray(const std::vector<uint8_t>& payload,
                             uint32_t width,
                             uint32_t height,
                             ConvertedImageFrame* frame,
                             std::string* error)
{
    // 一帧像素总数固定由宽高决定，后续所有缓冲区大小都基于这个值。
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t expected_payload_size = protocol::ComputeRaw16Low12PayloadSize(pixel_count);

    // 当前协议中每个像素占 2 字节，因此 payload 长度必须严格等于 width * height * 2。
    if (payload.size() != expected_payload_size)
    {
        if (error != NULL)
        {
            *error = "RAW16(low12 valid) payload size mismatch";
        }
        return false;
    }

    frame->width = width;
    frame->height = height;
    frame->pixels16.assign(pixel_count, 0u);
    frame->pixels8.assign(pixel_count, 0u);

    // 每个像素固定读取 2 字节。
    // 当前实现假设图像 payload 中的 16 位像素使用小端排列：
    // payload[2*i] 为低字节，payload[2*i+1] 为高字节。
    // 这与当前 FPGA 对 header 和 payload 统一使用小端传输的协议约定一致。
    for (std::size_t pixel_index = 0u; pixel_index < pixel_count; ++pixel_index)
    {
        const std::size_t payload_offset = pixel_index * sizeof(uint16_t);
        const uint16_t pixel16 = network::ReadUint16LE(payload.data() + payload_offset);

        // 只有低 12 位是真正的像素数据，高 4 位是 FPGA 侧补齐时产生的填充位。
        // 这里显式做掩码，避免上层误把高 4 位当成有效强度。
        const uint16_t valid12 = static_cast<uint16_t>(pixel16 & 0x0FFFu);

        // 16 位缓冲区保留“装在 16 位容器中的有效像素值”。
        // 这里存的是已经掩码后的值，也就是 0~4095 范围，后续做 CUDA 处理更直接。
        frame->pixels16[pixel_index] = valid12;

        // 12bit -> 8bit 使用整数比例缩放，输出可直接用于常规灰度显示。
        frame->pixels8[pixel_index] = static_cast<uint8_t>((static_cast<uint32_t>(valid12) * 255u) / 4095u);
    }

    return true;
}

}  // namespace image
}  // namespace spectra
