#include "service/SpectraBridgeClient.h"

#include <array>
#include <vector>

#include "image/ImageConverter.h"
#include "protocol/ControlProtocol.h"
#include "protocol/ImageProtocol.h"
#include "protocol/MessageTypes.h"
#include "util/Crc32.h"

namespace spectra {
namespace service {

namespace {

// 给图像 payload 设置上限，防止协议字段异常导致分配过大内存。
const std::size_t kMaxImagePayloadBytes = 16u * 1024u * 1024u;

}  // namespace

SpectraBridgeClient::SpectraBridgeClient(bridge::BridgeCallbacks* callbacks)
    : callbacks_(callbacks),
      running_(false)
{
    config_.verify_crc = true;
}

SpectraBridgeClient::~SpectraBridgeClient()
{
    Disconnect();
}

bool SpectraBridgeClient::Connect(const SpectraBridgeConfig& config, std::string* error)
{
    // Connect/Disconnect 是生命周期操作，必须串行。
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (running_.load())
    {
        if (error != NULL)
        {
            *error = "client is already running";
        }
        return false;
    }

    config_ = config;

    // 先连控制通道，再连图像通道；任一失败都立即回滚已建立的连接。
    if (!control_client_.Connect(config.control_endpoint.host, config.control_endpoint.port, error))
    {
        return false;
    }

    if (!image_client_.Connect(config.image_endpoint.host, config.image_endpoint.port, error))
    {
        control_client_.Close();
        return false;
    }

    running_.store(true);

    try
    {
        // 控制和图像分别独立线程处理，避免某一类流量阻塞另一类。
        control_thread_ = std::thread(&SpectraBridgeClient::ControlReceiveLoop, this);
        image_thread_ = std::thread(&SpectraBridgeClient::ImageReceiveLoop, this);
    }
    catch (...)
    {
        running_.store(false);
        control_client_.Close();
        image_client_.Close();
        if (control_thread_.joinable())
        {
            control_thread_.join();
        }
        if (image_thread_.joinable())
        {
            image_thread_.join();
        }
        if (error != NULL)
        {
            *error = "failed to create worker threads";
        }
        return false;
    }

    return true;
}

void SpectraBridgeClient::Disconnect()
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    // 先把运行标志置 false，再关闭 socket，阻塞中的 recv 会尽快返回。
    running_.store(false);
    control_client_.Close();
    image_client_.Close();

    if (control_thread_.joinable())
    {
        control_thread_.join();
    }

    if (image_thread_.joinable())
    {
        image_thread_.join();
    }
}

bool SpectraBridgeClient::SendReset(std::string* error)
{
    return SendCommand(protocol::ControlBits::RESET, error);
}

bool SpectraBridgeClient::SendTriggerOnce(std::string* error)
{
    return SendCommand(protocol::ControlBits::TRIGGER_ONCE, error);
}

bool SpectraBridgeClient::SendQueryStatus(std::string* error)
{
    // 查询状态请求只负责发送，结果异步由控制接收线程回调。
    const std::array<uint8_t, protocol::kQueryStatusPacketSize> packet = protocol::SerializeQueryStatusPacket();
    return SendControlBytes(packet.data(), packet.size(), error);
}

bool SpectraBridgeClient::SendFullConfig(
    const std::array<uint8_t, protocol::kFullConfigPayloadSize>& regs,
    std::string* error)
{
    // 配置包也只发送不等待，ConfigAckPacket 由后台线程异步接收。
    const std::vector<uint8_t> packet = protocol::SerializeFullConfigPacket(regs);
    return SendControlBytes(packet.data(), packet.size(), error);
}

bool SpectraBridgeClient::SendCommand(uint16_t control_bits, std::string* error)
{
    const std::array<uint8_t, protocol::kCommandPacketSize> packet = protocol::SerializeCommandPacket(control_bits);
    return SendControlBytes(packet.data(), packet.size(), error);
}

bool SpectraBridgeClient::SendControlBytes(const uint8_t* data, std::size_t size, std::string* error)
{
    // JNI 可能从多个 Java 线程并发调用发送接口，这里必须串行化 socket 写操作。
    std::lock_guard<std::mutex> lock(control_send_mutex_);

    if (!running_.load())
    {
        if (error != NULL)
        {
            *error = "client is not running";
        }
        return false;
    }

    return control_client_.SendAll(data, size, error);
}

void SpectraBridgeClient::ControlReceiveLoop()
{
    while (running_.load())
    {
        // 先读 1 字节消息类型，再根据类型决定还要补读多少字节。
        uint8_t msg_type = 0u;
        std::string error;
        if (!control_client_.RecvAll(&msg_type, 1u, &error))
        {
            if (running_.load())
            {
                HandleWorkerFailure("control", error);
            }
            return;
        }

        if (msg_type == static_cast<uint8_t>(protocol::MsgType::ReturnStatus))
        {
            // StatusPacket 总长度固定 5 字节，首字节已经读过，剩余再读 4 字节。
            std::array<uint8_t, protocol::kStatusPacketSize> wire_packet = {};
            wire_packet[0] = msg_type;
            if (!control_client_.RecvAll(wire_packet.data() + 1u, wire_packet.size() - 1u, &error))
            {
                HandleWorkerFailure("control", error);
                return;
            }

            protocol::StatusPacket packet;
            if (!protocol::ParseStatusPacket(wire_packet, &packet, &error))
            {
                HandleWorkerFailure("control", error);
                return;
            }

            if (callbacks_ != NULL)
            {
                // 交给 JNI 边界回调，不在这里耦合任何 Java 类型。
                callbacks_->OnStatusPacket(packet);
            }
            continue;
        }

        if (msg_type == static_cast<uint8_t>(protocol::MsgType::ConfigAck))
        {
            // ConfigAckPacket 同样是固定 5 字节格式。
            std::array<uint8_t, protocol::kConfigAckPacketSize> wire_packet = {};
            wire_packet[0] = msg_type;
            if (!control_client_.RecvAll(wire_packet.data() + 1u, wire_packet.size() - 1u, &error))
            {
                HandleWorkerFailure("control", error);
                return;
            }

            protocol::ConfigAckPacket packet;
            if (!protocol::ParseConfigAckPacket(wire_packet, &packet, &error))
            {
                HandleWorkerFailure("control", error);
                return;
            }

            if (callbacks_ != NULL)
            {
                callbacks_->OnConfigAckPacket(packet);
            }
            continue;
        }

        HandleWorkerFailure("control", "unsupported control reply type: " + std::to_string(msg_type));
        return;
    }
}

void SpectraBridgeClient::ImageReceiveLoop()
{
    while (running_.load())
    {
        // 图像通道固定先读基础头部，再按 header_len / payload_len 读后续数据。
        std::array<uint8_t, protocol::kImageFrameHeaderSize> wire_header = {};
        std::string error;
        if (!image_client_.RecvAll(wire_header.data(), wire_header.size(), &error))
        {
            if (running_.load())
            {
                HandleWorkerFailure("image", error);
            }
            return;
        }

        protocol::ImageFrameHeader header;
        if (!protocol::ParseImageFrameHeader(wire_header, &header, &error))
        {
            HandleWorkerFailure("image", error);
            return;
        }

        if (header.header_len > protocol::kImageFrameHeaderSize)
        {
            // 如果协议以后扩展头部字段，当前版本直接跳过扩展区，保持前向兼容。
            std::vector<uint8_t> extended_header(header.header_len - protocol::kImageFrameHeaderSize);
            if (!image_client_.RecvAll(extended_header.data(), extended_header.size(), &error))
            {
                HandleWorkerFailure("image", error);
                return;
            }
        }

        if (header.payload_len == 0u || header.payload_len > kMaxImagePayloadBytes)
        {
            HandleWorkerFailure("image", "payload_len is outside allowed range");
            return;
        }

        std::vector<uint8_t> payload(header.payload_len);
        if (!image_client_.RecvAll(payload.data(), payload.size(), &error))
        {
            HandleWorkerFailure("image", error);
            return;
        }

        if (config_.verify_crc)
        {
            // 可选 CRC 校验有助于尽早发现 FPGA 传输链路上的数据损坏。
            const uint32_t actual_crc = util::ComputeCrc32(payload.data(), payload.size());
            if (actual_crc != header.crc32)
            {
                HandleWorkerFailure("image", "payload CRC32 mismatch");
                return;
            }
        }

        image::ConvertedImageFrame frame;
        // 当前 FPGA 发送的是 16 位像素流，但只有低 12 位有效。
        // native 侧在这里统一完成掩码和 8 位显示图转换，JNI 层只接收整理后的结果。
        if (!image::ConvertRaw16Low12ToGray(payload, header.width, header.height, &frame, &error))
        {
            HandleWorkerFailure("image", error);
            return;
        }

        if (callbacks_ != NULL)
        {
            // 这里统一回调转换后的图像数据，后续 JNI 层可以决定传 8 位还是 16 位版本给 Java。
            callbacks_->OnImageFrameReady(frame);
        }
    }
}

void SpectraBridgeClient::HandleWorkerFailure(const std::string& channel, const std::string& message)
{
    // 任何一个工作线程出错，都认为当前会话不可继续，统一终止两条 TCP。
    const bool was_running = running_.exchange(false);
    control_client_.Close();
    image_client_.Close();

    if (was_running && callbacks_ != NULL)
    {
        callbacks_->OnTransportError(channel, message);
    }
}

}  // namespace service
}  // namespace spectra
