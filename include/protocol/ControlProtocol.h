#ifndef SPECTRA_BRIDGE_PROTOCOL_CONTROL_PROTOCOL_H
#define SPECTRA_BRIDGE_PROTOCOL_CONTROL_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "network/ByteUtils.h"
#include "protocol/MessageTypes.h"

namespace spectra {
namespace protocol {

// Java 侧发送控制请求时使用的命令包。
// 线上传输格式固定为: 1 字节类型 + 2 字节小端 param。
struct CommandPacket {
    uint8_t msg_type;
    uint16_t param;
};

// 主动查询 FPGA 状态时使用的请求包。
struct QueryStatusPacket {
    uint8_t msg_type;
    uint16_t reserved;
};

// 一次性下发完整寄存器配置的请求包。
// payload_len 固定为 512，regs 为裸寄存器镜像。
struct FullConfigPacket {
    uint8_t msg_type;
    uint16_t payload_len;
    std::array<uint8_t, 512> regs;
};

// FPGA 返回的状态包，通常对应控制结果或主动查询结果。
struct StatusPacket {
    uint8_t msg_type;
    uint16_t status_bits;
    uint16_t error_code;
};

// FPGA 返回的配置应答包，表示整包配置是否成功。
struct ConfigAckPacket {
    uint8_t msg_type;
    uint16_t result_code;
    uint16_t failed_addr;
};

// 各包固定大小，统一放到常量里，接收线程读包时直接使用。
constexpr std::size_t kCommandPacketSize = 3u;
constexpr std::size_t kQueryStatusPacketSize = 3u;
constexpr std::size_t kFullConfigPayloadSize = 512u;
constexpr std::size_t kFullConfigPacketSize = 515u;
constexpr std::size_t kStatusPacketSize = 5u;
constexpr std::size_t kConfigAckPacketSize = 5u;

// 将控制位打包成可直接发送到控制 TCP 的字节数组。
inline std::array<uint8_t, kCommandPacketSize> SerializeCommandPacket(uint16_t control_bits)
{
    std::array<uint8_t, kCommandPacketSize> buffer = {};
    buffer[0] = static_cast<uint8_t>(MsgType::Control);
    network::WriteUint16LE(control_bits, buffer.data() + 1);
    return buffer;
}

// 查询状态包没有业务负载，保留字段固定写 0。
inline std::array<uint8_t, kQueryStatusPacketSize> SerializeQueryStatusPacket()
{
    std::array<uint8_t, kQueryStatusPacketSize> buffer = {};
    buffer[0] = static_cast<uint8_t>(MsgType::QueryStatus);
    network::WriteUint16LE(0u, buffer.data() + 1);
    return buffer;
}

// 生成完整配置包。
// 这里返回 std::vector<uint8_t>，是因为配置包比简单控制包更长，且后续可能扩展。
inline std::vector<uint8_t> SerializeFullConfigPacket(const std::array<uint8_t, kFullConfigPayloadSize>& regs)
{
    std::vector<uint8_t> buffer;
    buffer.reserve(kFullConfigPacketSize);
    buffer.push_back(static_cast<uint8_t>(MsgType::Config));
    network::AppendUint16LE(&buffer, static_cast<uint16_t>(regs.size()));
    buffer.insert(buffer.end(), regs.begin(), regs.end());
    return buffer;
}

// 解析状态包时先验证消息类型，再做小端到主机字节序转换。
inline bool ParseStatusPacket(const std::array<uint8_t, kStatusPacketSize>& wire_packet,
                              StatusPacket* packet,
                              std::string* error)
{
    if (wire_packet[0] != static_cast<uint8_t>(MsgType::ReturnStatus))
    {
        if (error != NULL)
        {
            *error = "unexpected status packet type";
        }
        return false;
    }

    packet->msg_type = wire_packet[0];
    packet->status_bits = network::ReadUint16LE(wire_packet.data() + 1);
    packet->error_code = network::ReadUint16LE(wire_packet.data() + 3);
    return true;
}

// 解析配置应答包，failed_addr 仅在 result_code 表示失败时才有业务意义。
inline bool ParseConfigAckPacket(const std::array<uint8_t, kConfigAckPacketSize>& wire_packet,
                                 ConfigAckPacket* packet,
                                 std::string* error)
{
    if (wire_packet[0] != static_cast<uint8_t>(MsgType::ConfigAck))
    {
        if (error != NULL)
        {
            *error = "unexpected config ack packet type";
        }
        return false;
    }

    packet->msg_type = wire_packet[0];
    packet->result_code = network::ReadUint16LE(wire_packet.data() + 1);
    packet->failed_addr = network::ReadUint16LE(wire_packet.data() + 3);
    return true;
}

}  // namespace protocol
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_PROTOCOL_CONTROL_PROTOCOL_H
