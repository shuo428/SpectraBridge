#ifndef SPECTRA_BRIDGE_PROTOCOL_IMAGE_PROTOCOL_H
#define SPECTRA_BRIDGE_PROTOCOL_IMAGE_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
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

// 当前图像 payload 中每个像素固定占 2 字节。
// FPGA 已经在发送端把原始 12 位像素扩展到了 16 位容器中。
inline std::size_t ComputeRaw16Low12PayloadSize(std::size_t pixel_count)
{
    return pixel_count * sizeof(uint16_t);
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

    // 先验证魔数，避免把控制流中的其他数据误当作图像头处理。
    if (header->magic != kImageMagic)
    {
        if (error != NULL)
        {
            *error = "image frame magic mismatch";
        }
        return false;
    }

    // header_len 保留扩展能力，如果将来 FPGA 增加字段，旧代码仍可安全跳过扩展区。
    if (header->header_len < kImageFrameHeaderSize)
    {
        if (error != NULL)
        {
            *error = "image frame header_len is smaller than base header";
        }
        return false;
    }

    // 图像维度至少要合法，避免后续内存分配出现异常值。
    if (header->width == 0u || header->height == 0u)
    {
        if (error != NULL)
        {
            *error = "image frame width or height is zero";
        }
        return false;
    }

    return true;
}

}  // namespace protocol
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_PROTOCOL_IMAGE_PROTOCOL_H
