#ifndef SPECTRA_BRIDGE_PROTOCOL_MESSAGE_TYPES_H
#define SPECTRA_BRIDGE_PROTOCOL_MESSAGE_TYPES_H

#include <cstdint>

namespace spectra {
namespace protocol {

// 控制 TCP 上下行消息类型。
// Java/JNI/C++ 三层都应共享这一组值，避免各层硬编码。
enum class MsgType : uint8_t {
    Control = 0x01,
    QueryStatus = 0x02,
    Config = 0x03,
    ReturnStatus = 0x04,
    ConfigAck = 0x05
};

namespace ControlBits {
// 复位 FPGA 当前工作状态。
constexpr uint16_t RESET = 1u << 0;
// 触发一次单帧采集。
constexpr uint16_t TRIGGER_ONCE = 1u << 1;
}  // namespace ControlBits

}  // namespace protocol
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_PROTOCOL_MESSAGE_TYPES_H
