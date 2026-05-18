#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

// 默认监听端口。
// Java/JNI 客户端连接时，如果没有单独改端口，可以直接填这两个值联调。
constexpr uint16_t kDefaultControlPort = 5000;
constexpr uint16_t kDefaultImagePort = 5001;

// 当前模拟图像参数和 native 侧逻辑保持一致：
// 1. 图像固定 800x600；
// 2. 每像素占 16bit；
// 3. 只有低 12 位是有效数据；
// 4. header + payload 的所有多字节整数都按小端发送。
constexpr uint32_t kImageMagic = 0x494D4731u;  // "IMG1"
constexpr uint16_t kImageVersion = 1u;
constexpr uint16_t kImageHeaderLen = 32u;
constexpr uint32_t kImageWidth = 800u;
constexpr uint32_t kImageHeight = 600u;
constexpr uint32_t kImagePixelFormat = 0x00000010u;
constexpr std::size_t kImagePixelCount =
    static_cast<std::size_t>(kImageWidth) * static_cast<std::size_t>(kImageHeight);
constexpr std::size_t kImagePayloadSize = kImagePixelCount * sizeof(uint16_t);

namespace MsgType {
constexpr uint8_t CONTROL = 0x01;
constexpr uint8_t QUERY_STATUS = 0x02;
constexpr uint8_t CONFIG = 0x03;
constexpr uint8_t RETURN_STATUS = 0x04;
constexpr uint8_t CONFIG_ACK = 0x05;
}  // namespace MsgType

namespace ControlBits {
constexpr uint16_t RESET = 1u << 0;
constexpr uint16_t TRIGGER_ONCE = 1u << 1;
}  // namespace ControlBits

namespace StatusBits {
constexpr uint16_t BUSY = 1u << 0;
constexpr uint16_t READY = 1u << 1;
constexpr uint16_t ERROR_FLAG = 1u << 2;
constexpr uint16_t IMAGE_READY = 1u << 3;
constexpr uint16_t CONFIG_APPLIED = 1u << 4;
}  // namespace StatusBits

// 只用于回状态包的轻量快照。
// 不包含 mutex / condition_variable，因此可以安全拷贝。
struct StatusSnapshot {
    bool config_applied = false;
    bool image_ready = false;
    bool busy = false;
    uint16_t error_code = 0;
};

// 控制线程和图像线程共享的状态。
// 这里刻意保持简单，目标只是用于联调 JNI 和前端按钮，不追求逼真硬件时序。
struct SharedState {
    std::mutex mutex;
    std::condition_variable image_cv;

    bool running = true;
    bool trigger_pending = false;
    bool config_applied = false;
    bool image_ready = false;
    bool busy = false;
    uint16_t error_code = 0;
};

uint16_t ReadUint16LE(const uint8_t* data)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8));
}

void WriteUint16LE(uint16_t value, uint8_t* data)
{
    data[0] = static_cast<uint8_t>(value & 0xFFu);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void WriteUint32LE(uint32_t value, uint8_t* data)
{
    data[0] = static_cast<uint8_t>(value & 0xFFu);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint32_t ComputeCrc32(const uint8_t* data, std::size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (std::size_t index = 0u; index < size; ++index)
    {
        crc ^= static_cast<uint32_t>(data[index]);

        for (int bit = 0; bit < 8; ++bit)
        {
            const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

bool SendAll(SOCKET socket_handle, const uint8_t* data, std::size_t size)
{
    std::size_t total_sent = 0u;

    while (total_sent < size)
    {
        const int sent = send(socket_handle,
                              reinterpret_cast<const char*>(data + total_sent),
                              static_cast<int>(size - total_sent),
                              0);
        if (sent <= 0)
        {
            return false;
        }

        total_sent += static_cast<std::size_t>(sent);
    }

    return true;
}

bool RecvAll(SOCKET socket_handle, uint8_t* data, std::size_t size)
{
    std::size_t total_received = 0u;

    while (total_received < size)
    {
        const int received = recv(socket_handle,
                                  reinterpret_cast<char*>(data + total_received),
                                  static_cast<int>(size - total_received),
                                  0);
        if (received <= 0)
        {
            return false;
        }

        total_received += static_cast<std::size_t>(received);
    }

    return true;
}

SOCKET CreateListenSocket(uint16_t port)
{
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET)
    {
        return INVALID_SOCKET;
    }

    const BOOL reuse = 1;
    setsockopt(listen_socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(listen_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        closesocket(listen_socket);
        return INVALID_SOCKET;
    }

    if (listen(listen_socket, 1) != 0)
    {
        closesocket(listen_socket);
        return INVALID_SOCKET;
    }

    return listen_socket;
}

StatusSnapshot MakeStatusSnapshot(const SharedState& state)
{
    StatusSnapshot snapshot;
    snapshot.config_applied = state.config_applied;
    snapshot.image_ready = state.image_ready;
    snapshot.busy = state.busy;
    snapshot.error_code = state.error_code;
    return snapshot;
}

uint16_t BuildStatusBits(const StatusSnapshot& state)
{
    uint16_t bits = 0u;

    if (state.busy)
    {
        bits |= StatusBits::BUSY;
    }
    else
    {
        bits |= StatusBits::READY;
    }

    if (state.error_code != 0u)
    {
        bits |= StatusBits::ERROR_FLAG;
    }

    if (state.image_ready)
    {
        bits |= StatusBits::IMAGE_READY;
    }

    if (state.config_applied)
    {
        bits |= StatusBits::CONFIG_APPLIED;
    }

    return bits;
}

bool SendStatusPacket(SOCKET control_socket, const SharedState& state)
{
    const StatusSnapshot snapshot = MakeStatusSnapshot(state);

    std::array<uint8_t, 5> packet = {};
    packet[0] = MsgType::RETURN_STATUS;
    WriteUint16LE(BuildStatusBits(snapshot), packet.data() + 1);
    WriteUint16LE(snapshot.error_code, packet.data() + 3);
    return SendAll(control_socket, packet.data(), packet.size());
}

bool SendConfigAckPacket(SOCKET control_socket, uint16_t result_code, uint16_t failed_addr)
{
    std::array<uint8_t, 5> packet = {};
    packet[0] = MsgType::CONFIG_ACK;
    WriteUint16LE(result_code, packet.data() + 1);
    WriteUint16LE(failed_addr, packet.data() + 3);
    return SendAll(control_socket, packet.data(), packet.size());
}

std::vector<uint8_t> BuildImagePayload()
{
    std::vector<uint8_t> payload(kImagePayloadSize, 0u);

    // 构造一个容易识别的 12bit 图案：
    // 横向是主渐变，纵向叠加轻微变化。
    // 这样 Java 端显示出来时，不会是一张全黑或纯色图。
    for (uint32_t y = 0u; y < kImageHeight; ++y)
    {
        for (uint32_t x = 0u; x < kImageWidth; ++x)
        {
            const std::size_t pixel_index = static_cast<std::size_t>(y) * kImageWidth + x;
            const std::size_t payload_offset = pixel_index * sizeof(uint16_t);

            const uint16_t horizontal = static_cast<uint16_t>((x * 4095u) / (kImageWidth - 1u));
            const uint16_t vertical = static_cast<uint16_t>((y * 511u) / (kImageHeight - 1u));
            const uint16_t pixel12 = static_cast<uint16_t>((horizontal + vertical) & 0x0FFFu);

            // 每像素发送 16bit，小端，低 12 位有效。
            WriteUint16LE(pixel12, payload.data() + payload_offset);
        }
    }

    return payload;
}

bool SendOneImageFrame(SOCKET image_socket, SharedState& state)
{
    std::vector<uint8_t> payload = BuildImagePayload();
    const uint32_t payload_crc32 = ComputeCrc32(payload.data(), payload.size());

    std::array<uint8_t, 32> header = {};
    WriteUint32LE(kImageMagic, header.data() + 0);
    WriteUint16LE(kImageVersion, header.data() + 4);
    WriteUint16LE(kImageHeaderLen, header.data() + 6);
    WriteUint32LE(static_cast<uint32_t>(payload.size()), header.data() + 8);
    WriteUint32LE(kImageWidth, header.data() + 12);
    WriteUint32LE(kImageHeight, header.data() + 16);
    WriteUint32LE(kImagePixelFormat, header.data() + 20);
    WriteUint32LE(payload_crc32, header.data() + 24);
    WriteUint32LE(0u, header.data() + 28);

    if (!SendAll(image_socket, header.data(), header.size()))
    {
        return false;
    }

    if (!SendAll(image_socket, payload.data(), payload.size()))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.image_ready = true;
        state.busy = false;
    }

    return true;
}

void ControlThreadMain(SOCKET control_socket, SharedState& state)
{
    while (true)
    {
        uint8_t msg_type = 0u;
        if (!RecvAll(control_socket, &msg_type, 1u))
        {
            std::cout << "[MockFPGA] control client disconnected" << std::endl;
            return;
        }

        if (msg_type == MsgType::CONTROL)
        {
            std::array<uint8_t, 2> body = {};
            if (!RecvAll(control_socket, body.data(), body.size()))
            {
                return;
            }

            const uint16_t control_bits = ReadUint16LE(body.data());

            {
                std::lock_guard<std::mutex> lock(state.mutex);

                if ((control_bits & ControlBits::RESET) != 0u)
                {
                    state.trigger_pending = false;
                    state.config_applied = false;
                    state.image_ready = false;
                    state.busy = false;
                    state.error_code = 0u;
                    std::cout << "[MockFPGA] RESET received" << std::endl;
                }

                if ((control_bits & ControlBits::TRIGGER_ONCE) != 0u)
                {
                    state.trigger_pending = true;
                    state.image_ready = false;
                    state.busy = true;
                    std::cout << "[MockFPGA] TRIGGER_ONCE received" << std::endl;
                }
            }

            state.image_cv.notify_one();

            StatusSnapshot snapshot;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                snapshot = MakeStatusSnapshot(state);
            }

            std::array<uint8_t, 5> packet = {};
            packet[0] = MsgType::RETURN_STATUS;
            WriteUint16LE(BuildStatusBits(snapshot), packet.data() + 1);
            WriteUint16LE(snapshot.error_code, packet.data() + 3);
            if (!SendAll(control_socket, packet.data(), packet.size()))
            {
                return;
            }

            continue;
        }

        if (msg_type == MsgType::QUERY_STATUS)
        {
            std::array<uint8_t, 2> body = {};
            if (!RecvAll(control_socket, body.data(), body.size()))
            {
                return;
            }

            StatusSnapshot snapshot;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                snapshot = MakeStatusSnapshot(state);
            }

            std::cout << "[MockFPGA] QUERY_STATUS received" << std::endl;
            std::array<uint8_t, 5> packet = {};
            packet[0] = MsgType::RETURN_STATUS;
            WriteUint16LE(BuildStatusBits(snapshot), packet.data() + 1);
            WriteUint16LE(snapshot.error_code, packet.data() + 3);
            if (!SendAll(control_socket, packet.data(), packet.size()))
            {
                return;
            }

            continue;
        }

        if (msg_type == MsgType::CONFIG)
        {
            std::array<uint8_t, 2> payload_len_bytes = {};
            if (!RecvAll(control_socket, payload_len_bytes.data(), payload_len_bytes.size()))
            {
                return;
            }

            const uint16_t payload_len = ReadUint16LE(payload_len_bytes.data());
            std::vector<uint8_t> regs(payload_len, 0u);

            if (!RecvAll(control_socket, regs.data(), regs.size()))
            {
                return;
            }

            std::cout << "[MockFPGA] CONFIG received, payload_len=" << payload_len << std::endl;

            const bool config_ok = (payload_len == 512u);
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.config_applied = config_ok;
                state.error_code = config_ok ? 0u : 1u;
            }

            if (!SendConfigAckPacket(control_socket,
                                     config_ok ? 0u : 1u,
                                     config_ok ? 0u : 0x0001u))
            {
                return;
            }

            continue;
        }

        std::cout << "[MockFPGA] unsupported msg_type=" << static_cast<int>(msg_type) << std::endl;
        return;
    }
}

void ImageThreadMain(SOCKET image_socket, SharedState& state)
{
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.image_cv.wait(lock, [&state] {
                return !state.running || state.trigger_pending;
            });

            if (!state.running)
            {
                return;
            }

            state.trigger_pending = false;
        }

        std::cout << "[MockFPGA] sending one image frame" << std::endl;
        if (!SendOneImageFrame(image_socket, state))
        {
            std::cout << "[MockFPGA] image client disconnected" << std::endl;
            return;
        }
    }
}

}  // namespace

int main()
{
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        std::cerr << "[MockFPGA] WSAStartup failed" << std::endl;
        return 1;
    }

    SOCKET control_listen_socket = CreateListenSocket(kDefaultControlPort);
    SOCKET image_listen_socket = CreateListenSocket(kDefaultImagePort);

    if (control_listen_socket == INVALID_SOCKET || image_listen_socket == INVALID_SOCKET)
    {
        std::cerr << "[MockFPGA] failed to create listen sockets" << std::endl;

        if (control_listen_socket != INVALID_SOCKET)
        {
            closesocket(control_listen_socket);
        }
        if (image_listen_socket != INVALID_SOCKET)
        {
            closesocket(image_listen_socket);
        }

        WSACleanup();
        return 1;
    }

    std::cout << "[MockFPGA] waiting for control connection on port "
              << kDefaultControlPort << std::endl;
    SOCKET control_socket = accept(control_listen_socket, NULL, NULL);
    if (control_socket == INVALID_SOCKET)
    {
        std::cerr << "[MockFPGA] accept(control) failed" << std::endl;
        closesocket(control_listen_socket);
        closesocket(image_listen_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[MockFPGA] control connected" << std::endl;

    std::cout << "[MockFPGA] waiting for image connection on port "
              << kDefaultImagePort << std::endl;
    SOCKET image_socket = accept(image_listen_socket, NULL, NULL);
    if (image_socket == INVALID_SOCKET)
    {
        std::cerr << "[MockFPGA] accept(image) failed" << std::endl;
        closesocket(control_socket);
        closesocket(control_listen_socket);
        closesocket(image_listen_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[MockFPGA] image connected" << std::endl;

    SharedState state;

    std::thread control_thread(ControlThreadMain, control_socket, std::ref(state));
    std::thread image_thread(ImageThreadMain, image_socket, std::ref(state));

    control_thread.join();

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.running = false;
    }
    state.image_cv.notify_all();

    shutdown(image_socket, SD_BOTH);
    closesocket(image_socket);
    image_thread.join();

    closesocket(control_socket);
    closesocket(control_listen_socket);
    closesocket(image_listen_socket);
    WSACleanup();

    std::cout << "[MockFPGA] stopped" << std::endl;
    return 0;
}
