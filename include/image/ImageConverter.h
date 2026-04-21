#ifndef SPECTRA_BRIDGE_IMAGE_IMAGE_CONVERTER_H
#define SPECTRA_BRIDGE_IMAGE_IMAGE_CONVERTER_H

#include <cstdint>
#include <string>
#include <vector>

namespace spectra {
namespace image {

// 一帧转换后的图像结果。
// 这里同时保留 16 位和 8 位版本，便于后续 JNI 或 CUDA 按需选择。
// 注意：
// 1. pixels16 中保存的是“已经扩展到 16 位容器中的像素值”；
// 2. 当前协议下只有低 12 位有效，高 4 位只是填充位。
struct ConvertedImageFrame {
    uint32_t width;
    uint32_t height;
    std::vector<uint16_t> pixels16;
    std::vector<uint8_t> pixels8;
};

// 将 FPGA 发来的“每像素 16 位、低 12 位有效”的 payload 转换成：
// 1. 16 位灰度图，保留每个像素当前 16 位容器内的数值；
// 2. 8 位灰度图，使用低 12 位有效数据缩放到 0~255，便于 Java 或界面层直接显示。
//
// 约定：
// 1. payload 中每个像素占 2 字节；
// 2. 每个像素按小端读取，与协议头的字节序保持一致；
// 3. 真正有效的数据是像素值的低 12 位。
bool ConvertRaw16Low12ToGray(const std::vector<uint8_t>& payload,
                             uint32_t width,
                             uint32_t height,
                             ConvertedImageFrame* frame,
                             std::string* error);

}  // namespace image
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_IMAGE_IMAGE_CONVERTER_H
