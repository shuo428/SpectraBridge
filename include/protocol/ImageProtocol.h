#ifndef SPECTRA_BRIDGE_PROTOCOL_IMAGE_PROTOCOL_H
#define SPECTRA_BRIDGE_PROTOCOL_IMAGE_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "network/ByteUtils.h"

namespace spectra {
namespace protocol {

// 图像 TCP 每一帧的固定头部。
// 线上顺序为 header + payload，其中所有多字节整数均按小端传输。
struct ImageFrameHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint32_t payload_len;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t crc32;
    uint32_t reserved;
};

constexpr uint32_t kImageMagic = 0x494D4731u;
constexpr uint16_t kImageProtocolVersion = 1u;
constexpr std::size_t kImageFrameHeaderSize = 32u;
constexpr std::size_t kMaxImageFrameHeaderSize = 256u;

// 当前 FPGA 约定的像素格式值：
// 每个像素在线路上占 16 bit，只有低 12 bit 是有效灰度值。
constexpr uint32_t kPixelFormatRaw16Low12 = 0x00000010u;

// 给 native 错误增加稳定的机器码。Java 层会读取方括号中的代码，
// 将失败原因写入 t_image_integrity_analysis.result_code。
inline void SetImageProtocolError(std::string* error,
                                  const std::string& code,
                                  const std::string& message)
{
    if (error != NULL)
    {
        *error = "[" + code + "] " + message;
    }
}

// 安全计算 RAW16(low12 valid) 图像所需的 payload 长度。
// 这里不能直接 width * height * 2：协议头来自网络，异常的大尺寸可能造成整数溢出，
// 继而绕过长度检查并导致错误的内存分配。
inline bool TryComputeRaw16Low12PayloadSize(uint32_t width,
                                           uint32_t height,
                                           std::size_t* payload_size)
{
    if (payload_size == NULL || width == 0u || height == 0u)
    {
        return false;
    }

    const std::size_t width_value = static_cast<std::size_t>(width);
    const std::size_t height_value = static_cast<std::size_t>(height);
    if (width_value > std::numeric_limits<std::size_t>::max() / height_value)
    {
        return false;
    }

    const std::size_t pixel_count = width_value * height_value;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / sizeof(uint16_t))
    {
        return false;
    }

    *payload_size = pixel_count * sizeof(uint16_t);
    return true;
}

// 从网络收到的原始头部字节中解析出主机侧结构体。
// 此函数只做通用合法性检查，不绑定具体像素格式枚举。
inline bool ParseImageFrameHeader(const std::array<uint8_t, kImageFrameHeaderSize>& wire_header,
                                  ImageFrameHeader* header,
                                  std::string* error)
{
    header->magic = network::ReadUint32LE(wire_header.data());
    header->version = network::ReadUint16LE(wire_header.data() + 4);
    header->header_len = network::ReadUint16LE(wire_header.data() + 6);
    header->payload_len = network::ReadUint32LE(wire_header.data() + 8);
    header->width = network::ReadUint32LE(wire_header.data() + 12);
    header->height = network::ReadUint32LE(wire_header.data() + 16);
    header->pixel_format = network::ReadUint32LE(wire_header.data() + 20);
    header->crc32 = network::ReadUint32LE(wire_header.data() + 24);
    header->reserved = network::ReadUint32LE(wire_header.data() + 28);

    // 先验证魔数，避免把其他字节流误当成图像头。
    if (header->magic != kImageMagic)
    {
        SetImageProtocolError(error, "INVALID_MAGIC", "image frame magic mismatch");
        return false;
    }

    // 当前实现只理解版本 1。未知版本不能继续“猜测”字段含义，否则后续长度和像素
    // 解析都可能错误。
    if (header->version != kImageProtocolVersion)
    {
        SetImageProtocolError(error, "UNSUPPORTED_VERSION", "unsupported image protocol version");
        return false;
    }

    // 允许协议在基础 32 字节之后增加少量扩展字段，但设置 256 字节上限，
    // 防止损坏的 header_len 触发大块无意义内存分配。
    if (header->header_len < kImageFrameHeaderSize ||
        header->header_len > kMaxImageFrameHeaderSize)
    {
        SetImageProtocolError(error, "INVALID_HEADER_LENGTH", "image frame header_len is outside allowed range");
        return false;
    }

    // 零尺寸一定不是合法图像。具体是否等于当前配置的 800x600，
    // 在 service 层结合当前采集配置继续检查。
    if (header->width == 0u || header->height == 0u)
    {
        SetImageProtocolError(error, "INVALID_DIMENSIONS", "image frame width or height is zero");
        return false;
    }

    if (header->pixel_format != kPixelFormatRaw16Low12)
    {
        SetImageProtocolError(error, "INVALID_PIXEL_FORMAT", "unsupported image pixel format");
        return false;
    }

    // reserved 在版本 1 中必须为 0。非零通常意味着 FPGA 和上位机协议版本不一致。
    if (header->reserved != 0u)
    {
        SetImageProtocolError(error, "INVALID_RESERVED_FIELD", "image frame reserved field must be zero");
        return false;
    }

    std::size_t expected_payload_size = 0u;
    if (!TryComputeRaw16Low12PayloadSize(header->width, header->height, &expected_payload_size))
    {
        SetImageProtocolError(error, "INVALID_DIMENSIONS", "image dimensions overflow payload size calculation");
        return false;
    }

    const std::size_t payload_len = static_cast<std::size_t>(header->payload_len);
    const bool single_plane_payload = payload_len == expected_payload_size;
    const bool hdr_two_plane_payload =
        expected_payload_size <= std::numeric_limits<std::size_t>::max() / 2u &&
        payload_len == expected_payload_size * 2u;
    if (!single_plane_payload && !hdr_two_plane_payload)
    {
        SetImageProtocolError(
            error,
            "PAYLOAD_LENGTH_MISMATCH",
            "payload_len must equal one RAW16 plane or two RAW16 planes(HG followed by LG)");
        return false;
    }

    return true;
}

}  // namespace protocol
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_PROTOCOL_IMAGE_PROTOCOL_H
