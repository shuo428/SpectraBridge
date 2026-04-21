#ifndef SPECTRA_BRIDGE_NETWORK_BYTE_UTILS_H
#define SPECTRA_BRIDGE_NETWORK_BYTE_UTILS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spectra {
namespace network {

// 网络协议要求所有多字节整数均使用小端序传输。
// 这里统一提供读写工具，避免在业务代码中重复位运算。
inline uint16_t ReadUint16LE(const uint8_t* data)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8));
}

// 从连续 4 字节缓冲区中读取一个小端 32 位无符号整数。
inline uint32_t ReadUint32LE(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

// 将主机字节序的 16 位整数写为网络协议使用的小端布局。
inline void WriteUint16LE(uint16_t value, uint8_t* data)
{
    data[0] = static_cast<uint8_t>(value & 0xFFu);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

// 将主机字节序的 32 位整数写为网络协议使用的小端布局。
inline void WriteUint32LE(uint32_t value, uint8_t* data)
{
    data[0] = static_cast<uint8_t>(value & 0xFFu);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

// 追加一个小端 16 位整数到动态缓冲区末尾，常用于构造控制报文。
inline void AppendUint16LE(std::vector<uint8_t>* buffer, uint16_t value)
{
    const std::size_t original_size = buffer->size();
    buffer->resize(original_size + 2u);
    WriteUint16LE(value, buffer->data() + original_size);
}

// 追加一个小端 32 位整数到动态缓冲区末尾，便于后续扩展协议。
inline void AppendUint32LE(std::vector<uint8_t>* buffer, uint32_t value)
{
    const std::size_t original_size = buffer->size();
    buffer->resize(original_size + 4u);
    WriteUint32LE(value, buffer->data() + original_size);
}

}  // namespace network
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_NETWORK_BYTE_UTILS_H
