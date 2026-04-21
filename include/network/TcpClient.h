#ifndef SPECTRA_BRIDGE_NETWORK_TCP_CLIENT_H
#define SPECTRA_BRIDGE_NETWORK_TCP_CLIENT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace spectra {
namespace network {

// 轻量级阻塞式 TCP 客户端封装。
// 这里不引入异步 IO 框架，目的是保持 JNI 对接前的实现简单、清晰、可控。
class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    // 建立到目标主机的 TCP 连接。
    bool Connect(const std::string& host, uint16_t port, std::string* error);
    // 关闭连接；可安全重复调用。
    void Close();
    bool IsConnected() const;

    // 保证完整发送 size 字节数据，否则返回失败。
    bool SendAll(const uint8_t* data, std::size_t size, std::string* error);
    // 保证完整接收 size 字节数据，否则返回失败。
    bool RecvAll(uint8_t* data, std::size_t size, std::string* error);

private:
    static std::uintptr_t InvalidSocketValue();
    static bool EnsureSocketRuntime(std::string* error);
    static std::string BuildLastSocketErrorMessage(const std::string& prefix);

    std::atomic<std::uintptr_t> socket_handle_;
};

}  // namespace network
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_NETWORK_TCP_CLIENT_H
