#ifndef SPECTRA_BRIDGE_SERVICE_SPECTRA_BRIDGE_CLIENT_H
#define SPECTRA_BRIDGE_SERVICE_SPECTRA_BRIDGE_CLIENT_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "bridge/BridgeCallbacks.h"
#include "network/TcpClient.h"
#include "protocol/ControlProtocol.h"

namespace spectra {
namespace service {

// 单个 TCP 连接的地址信息。
struct TcpEndpoint {
    std::string host;
    uint16_t port;
};

// Native 侧桥接客户端配置。
// 当前拆成控制通道和图像通道两条 TCP，便于互不阻塞。
struct SpectraBridgeConfig {
    TcpEndpoint control_endpoint;
    TcpEndpoint image_endpoint;
    // CRC 校验默认开启，调试协议时如需临时关闭可以通过配置控制。
    bool verify_crc;
    // 当前项目使用 GLUX1605BSI 的 800x600 有效区域。
    // 将预期尺寸放入配置而不是散落在接收循环中，后续切换 ROI 时只需调整配置来源。
    uint32_t expected_width;
    uint32_t expected_height;
    uint32_t expected_pixel_format;
};

// 整个 native 采集桥的核心服务类。
// 设计目标：
// 1. Java/JNI 线程只做发送，不等待 FPGA 返回；
// 2. 控制结果和图像接收分别由独立线程处理；
// 3. 通过回调接口向未来的 JNI 层交付结果。
class SpectraBridgeClient {
public:
    explicit SpectraBridgeClient(bridge::BridgeCallbacks* callbacks);
    ~SpectraBridgeClient();

    // 同时建立控制 TCP 和图像 TCP，并启动两个接收线程。
    bool Connect(const SpectraBridgeConfig& config, std::string* error);
    // 停止工作线程并关闭两条连接。
    void Disconnect();

    // 以下几个接口设计给 JNI 直接调用，符合“只发不等”的要求。
    bool SendReset(std::string* error);
    bool SendTriggerOnce(std::string* error);
    bool SendQueryStatus(std::string* error);
    bool SendFullConfig(const std::array<uint8_t, protocol::kFullConfigPayloadSize>& regs, std::string* error);

private:
    // 将具体控制位组合成控制包后复用底层发送流程。
    bool SendCommand(uint16_t control_bits, std::string* error);
    // 控制 TCP 上所有发送都走这里，并用互斥锁保证线程安全。
    bool SendControlBytes(const uint8_t* data, std::size_t size, std::string* error);

    // 控制线程：接收 StatusPacket / ConfigAckPacket。
    void ControlReceiveLoop();
    // 图像线程：接收 ImageFrameHeader + payload，并完成图像转换。
    void ImageReceiveLoop();
    // 任一工作线程失败时统一关闭所有资源，并通过回调上报错误。
    void HandleWorkerFailure(const std::string& channel, const std::string& message);

    bridge::BridgeCallbacks* callbacks_;
    SpectraBridgeConfig config_;
    network::TcpClient control_client_;
    network::TcpClient image_client_;
    // running_ 是两个工作线程的总开关。
    std::atomic<bool> running_;
    // 保护连接建立/断开流程，防止重复 Connect/Disconnect 交叉执行。
    std::mutex lifecycle_mutex_;
    // 保护控制通道发送，避免多个 JNI 调用并发写 socket 导致报文交叉。
    std::mutex control_send_mutex_;
    std::thread control_thread_;
    std::thread image_thread_;
};

}  // namespace service
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_SERVICE_SPECTRA_BRIDGE_CLIENT_H
