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
    // FPGA 直接发来的原始有效像素 payload，保持原始读出顺序，不做重排。
    // Java 侧会把它保存为 fpga_payload.bin，便于后续回放、排错和验证重排算法。
    std::vector<uint8_t> fpga_payload;
    // 记录本帧采用的读出顺序，便于持久化层写入审计信息。
    std::string readout_order;
    std::vector<uint16_t> pixels16;
    std::vector<uint8_t> pixels8;
};

// HDR 一次触发返回的双增益结果。
//
// 当前约定：
// 1. 同一个图像包里先发送完整 HG 平面，再发送完整 LG 平面；
// 2. HG 和 LG 每个平面内部仍使用当前连接配置指定的读出顺序；
// 3. C++ 只负责拆平面和空间重排，不在 native 层做增益融合，避免把定量策略固化在 DLL 中。
struct ConvertedHdrImageFrame {
    uint32_t width;
    uint32_t height;
    // FPGA 一次触发直接发来的原始双平面 payload，顺序为 HG plane + LG plane。
    std::vector<uint8_t> fpga_payload;
    std::string readout_order;
    std::vector<uint16_t> hg_pixels16;
    std::vector<uint16_t> lg_pixels16;
};

enum class ReadoutOrder {
    // payload 已经是常规 row-major：0,1,2,3...
    kRowMajor,
    // GLUX1605BSI HDR 模式 Figure 42 中的纯有效像素、4 个 Sub-LVDS 通道交织顺序：
    // 0, laneWidth, 2*laneWidth, 3*laneWidth, 1, laneWidth+1...
    // 注意：这里处理的是 FPGA 已经去掉 SOF/SOL、EOF/EOL、blank、dummy 后的一张有效图。
    // 如果后续 FPGA 同时发送 HG+LG 两个平面，需要在更高层先拆平面，再分别调用该重排。
    kGlux1605Hdr4LaneInterleaved
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
                             ReadoutOrder readout_order,
                             ConvertedImageFrame* frame,
                             std::string* error);

// 将“一次触发得到的 HG+LG 双平面 payload”拆成两张正常行列顺序的 RAW16 图。
//
// payload 长度必须严格等于 width * height * 2 * 2：
// - 前半段：HG 的完整有效像素平面；
// - 后半段：LG 的完整有效像素平面。
bool ConvertHdrRaw16Low12ToPlanes(const std::vector<uint8_t>& payload,
                                  uint32_t width,
                                  uint32_t height,
                                  ReadoutOrder readout_order,
                                  ConvertedHdrImageFrame* frame,
                                  std::string* error);

}  // namespace image
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_IMAGE_IMAGE_CONVERTER_H
