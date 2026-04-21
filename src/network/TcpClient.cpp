#include "network/TcpClient.h"

#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace spectra {
namespace network {

namespace {

// connect/send/recv 的地址长度参数在不同平台类型不同，这里做统一别名。
#ifdef _WIN32
typedef int SockLenType;
#else
typedef socklen_t SockLenType;
#endif

// 用统一常量表达“无效 socket”，便于 TcpClient 在 Windows/Unix 上复用。
#ifdef _WIN32
const std::uintptr_t kInvalidSocketValue = static_cast<std::uintptr_t>(INVALID_SOCKET);
#else
const std::uintptr_t kInvalidSocketValue = static_cast<std::uintptr_t>(-1);
#endif

// 本地辅助关闭函数，不抛异常，也不关心关闭失败。
void CloseNativeSocket(std::uintptr_t handle)
{
    if (handle == kInvalidSocketValue)
    {
        return;
    }

#ifdef _WIN32
    closesocket(static_cast<SOCKET>(handle));
#else
    close(static_cast<int>(handle));
#endif
}

}  // namespace

TcpClient::TcpClient() : socket_handle_(InvalidSocketValue())
{
}

TcpClient::~TcpClient()
{
    Close();
}

bool TcpClient::Connect(const std::string& host, uint16_t port, std::string* error)
{
    // 允许重复调用 Connect；每次重连前先确保旧连接被释放。
    Close();

    if (!EnsureSocketRuntime(error))
    {
        return false;
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::ostringstream port_stream;
    port_stream << port;

    struct addrinfo* result = NULL;
    const int getaddrinfo_result = getaddrinfo(host.c_str(), port_stream.str().c_str(), &hints, &result);
    if (getaddrinfo_result != 0)
    {
        if (error != NULL)
        {
#ifdef _WIN32
            *error = "getaddrinfo failed with code " + std::to_string(getaddrinfo_result);
#else
            *error = gai_strerror(getaddrinfo_result);
#endif
        }
        return false;
    }

    bool connected = false;
    for (struct addrinfo* current = result; current != NULL; current = current->ai_next)
    {
        // 逐个尝试解析结果，兼容 IPv4 / IPv6。
        const std::uintptr_t native_socket =
            static_cast<std::uintptr_t>(socket(current->ai_family, current->ai_socktype, current->ai_protocol));

        if (native_socket == InvalidSocketValue())
        {
            continue;
        }

#ifdef _WIN32
        const int connect_result = connect(static_cast<SOCKET>(native_socket), current->ai_addr,
                                           static_cast<SockLenType>(current->ai_addrlen));
#else
        const int connect_result = connect(static_cast<int>(native_socket), current->ai_addr,
                                           static_cast<SockLenType>(current->ai_addrlen));
#endif

        if (connect_result == 0)
        {
            // 只有成功建立连接后才把 socket 句柄交给成员变量托管。
            socket_handle_.store(native_socket);
            connected = true;
            break;
        }

        CloseNativeSocket(native_socket);
    }

    freeaddrinfo(result);

    if (!connected && error != NULL)
    {
        *error = BuildLastSocketErrorMessage("failed to connect");
    }

    return connected;
}

void TcpClient::Close()
{
    // exchange 保证多线程场景下只会真正关闭一次同一个 socket。
    const std::uintptr_t previous = socket_handle_.exchange(InvalidSocketValue());
    CloseNativeSocket(previous);
}

bool TcpClient::IsConnected() const
{
    return socket_handle_.load() != InvalidSocketValue();
}

bool TcpClient::SendAll(const uint8_t* data, std::size_t size, std::string* error)
{
    const std::uintptr_t handle = socket_handle_.load();
    if (handle == InvalidSocketValue())
    {
        if (error != NULL)
        {
            *error = "socket is not connected";
        }
        return false;
    }

    std::size_t total_sent = 0u;
    while (total_sent < size)
    {
        // TCP 是流，不保证一次 send 就把整包写完，因此循环直至全部送出。
#ifdef _WIN32
        const int bytes_sent = send(static_cast<SOCKET>(handle),
                                    reinterpret_cast<const char*>(data + total_sent),
                                    static_cast<int>(size - total_sent),
                                    0);
#else
        const int bytes_sent = send(static_cast<int>(handle),
                                    reinterpret_cast<const char*>(data + total_sent),
                                    size - total_sent,
                                    0);
#endif
        if (bytes_sent <= 0)
        {
            if (error != NULL)
            {
                *error = BuildLastSocketErrorMessage("failed to send data");
            }
            return false;
        }

        total_sent += static_cast<std::size_t>(bytes_sent);
    }

    return true;
}

bool TcpClient::RecvAll(uint8_t* data, std::size_t size, std::string* error)
{
    const std::uintptr_t handle = socket_handle_.load();
    if (handle == InvalidSocketValue())
    {
        if (error != NULL)
        {
            *error = "socket is not connected";
        }
        return false;
    }

    std::size_t total_received = 0u;
    while (total_received < size)
    {
        // TCP 不保留消息边界，所以必须按预期长度反复 recv，直到凑够完整报文。
#ifdef _WIN32
        const int bytes_received = recv(static_cast<SOCKET>(handle),
                                        reinterpret_cast<char*>(data + total_received),
                                        static_cast<int>(size - total_received),
                                        0);
#else
        const int bytes_received = recv(static_cast<int>(handle),
                                        reinterpret_cast<char*>(data + total_received),
                                        size - total_received,
                                        0);
#endif
        if (bytes_received <= 0)
        {
            if (error != NULL)
            {
                *error = BuildLastSocketErrorMessage("failed to receive data");
            }
            return false;
        }

        total_received += static_cast<std::size_t>(bytes_received);
    }

    return true;
}

std::uintptr_t TcpClient::InvalidSocketValue()
{
    return kInvalidSocketValue;
}

bool TcpClient::EnsureSocketRuntime(std::string* error)
{
#ifdef _WIN32
    // Winsock 需要进程级初始化一次，后续连接可以重复使用。
    static bool initialized = false;
    static bool initialization_ok = false;

    if (!initialized)
    {
        initialized = true;
        WSADATA data;
        initialization_ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }

    if (!initialization_ok)
    {
        if (error != NULL)
        {
            *error = "WSAStartup failed";
        }
        return false;
    }
#else
    (void)error;
#endif

    return true;
}

std::string TcpClient::BuildLastSocketErrorMessage(const std::string& prefix)
{
    // 错误信息保持简洁，只提供定位所需的系统错误码。
#ifdef _WIN32
    return prefix + ", WSA error=" + std::to_string(WSAGetLastError());
#else
    return prefix + ", errno=" + std::to_string(errno);
#endif
}

}  // namespace network
}  // namespace spectra
