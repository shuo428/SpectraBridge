#include "bridge/JniBridgePlaceholder.h"

#include <iostream>

namespace spectra {
namespace bridge {

void JniBridgePlaceholder::OnImageFrameReady(const image::ConvertedImageFrame& frame)
{
    // 当前只是占位输出。
    // 真正接 JNI 时，这里通常会涉及 AttachCurrentThread、数组分配和 Java 回调，
    // 因此先用互斥锁保护日志，避免多个工作线程输出互相穿插。
    std::lock_guard<std::mutex> lock(print_mutex_);

    // TODO:
    // 1. 在 JNI 层缓存 Java VM 和回调对象的全局引用。
    // 2. 将 pixels8 或 pixels16 拷贝到 jbyteArray/jshortArray。
    // 3. 调用 Java 回调，把图像宽高和数据传回上层。
    std::cout << "[JNI TODO] image frame received: "
              << frame.width << "x" << frame.height
              << ", gray8 bytes=" << frame.pixels8.size()
              << ", gray16 pixels=" << frame.pixels16.size()
              << std::endl;
}

void JniBridgePlaceholder::OnStatusPacket(const protocol::StatusPacket& packet)
{
    // 状态包来自控制接收线程，后续 JNI 实现时要注意线程附着到 JVM。
    std::lock_guard<std::mutex> lock(print_mutex_);

    // TODO: JNI 回调 Java，回传 status_bits 和 error_code。
    std::cout << "[JNI TODO] status packet: status_bits="
              << packet.status_bits
              << ", error_code=" << packet.error_code
              << std::endl;
}

void JniBridgePlaceholder::OnConfigAckPacket(const protocol::ConfigAckPacket& packet)
{
    // 配置结果通常紧跟配置下发动作返回，这里保留和状态包相同的 JNI 扩展点。
    std::lock_guard<std::mutex> lock(print_mutex_);

    // TODO: JNI 回调 Java，回传配置是否成功以及失败地址。
    std::cout << "[JNI TODO] config ack: result_code="
              << packet.result_code
              << ", failed_addr=" << packet.failed_addr
              << std::endl;
}

void JniBridgePlaceholder::OnTransportError(const std::string& channel, const std::string& message)
{
    // 任一 socket 或协议解析失败都会走到这里，便于 Java 层统一感知链路异常。
    std::lock_guard<std::mutex> lock(print_mutex_);
    std::cerr << "[TransportError][" << channel << "] " << message << std::endl;
}

}  // namespace bridge
}  // namespace spectra
