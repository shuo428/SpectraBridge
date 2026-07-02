#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "network/ByteUtils.h"
#include "protocol/ControlProtocol.h"
#include "protocol/ImageProtocol.h"
#include "util/Crc32.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

// spectra_bridge_test.exe 现在作为“轻量 Mock FPGA”使用：
// 1. Java/SpringBoot 仍然通过 DLL 连接 127.0.0.1:5000 / 5001；
// 2. DLL 的 SpectraBridgeClient 会像连接真实 FPGA 一样接收 header + payload；
// 3. 因此 ParseImageFrameHeader、payload_len、CRC、RAW12 高位检查都会正常执行；
// 4. Java 只有在 native 完整性检查通过后才会收到 onImageFrame(...)。
//
// 这和直接在测试程序里调用 Java 回调不同：这里不绕过任何接收完整性逻辑。

constexpr uint16_t kDefaultControlPort = 5000;
constexpr uint16_t kDefaultImagePort = 5001;

constexpr uint32_t kImageWidth = 800u;
constexpr uint32_t kImageHeight = 600u;
constexpr std::size_t kImagePixelCount =
    static_cast<std::size_t>(kImageWidth) * static_cast<std::size_t>(kImageHeight);
constexpr std::size_t kImagePayloadSize = kImagePixelCount * sizeof(uint16_t);

namespace StatusBits {
constexpr uint16_t BUSY = 1u << 0;
constexpr uint16_t READY = 1u << 1;
constexpr uint16_t ERROR_FLAG = 1u << 2;
constexpr uint16_t IMAGE_READY = 1u << 3;
constexpr uint16_t CONFIG_APPLIED = 1u << 4;
}  // namespace StatusBits

struct ProgramOptions {
    uint16_t control_port = kDefaultControlPort;
    uint16_t image_port = kDefaultImagePort;
    std::string image_path;
    bool self_test = false;
};

struct TestImage {
    std::string name;
    std::vector<uint8_t> pixels8;
};

struct StatusSnapshot {
    bool config_applied = false;
    bool image_ready = false;
    bool busy = false;
    uint16_t error_code = 0;
};

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

std::string NormalizePathForLog(const std::string& path)
{
    return path.empty() ? "(empty)" : path;
}

bool ParseUint16(const std::string& value, uint16_t* output)
{
    if (output == NULL)
    {
        return false;
    }

    try
    {
        int parsed = std::stoi(value);
        if (parsed < 1 || parsed > 65535)
        {
            return false;
        }
        *output = static_cast<uint16_t>(parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void PrintUsage()
{
    std::cout
        << "Usage:\n"
        << "  spectra_bridge_test.exe [--control-port 5000] [--image-port 5001] [--image path.pgm]\n"
        << "  spectra_bridge_test.exe --self-test\n\n"
        << "If --image is omitted, the program cycles through an in-memory PASS,\n"
        << "WARNING and FAIL spectrum image bank, one image for each trigger.\n";
}

bool ParseOptions(int argc, char** argv, ProgramOptions* options)
{
    if (options == NULL)
    {
        return false;
    }

    for (int index = 1; index < argc; ++index)
    {
        std::string arg = argv[index];
        if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return false;
        }
        if (arg == "--self-test")
        {
            options->self_test = true;
            continue;
        }
        if (arg == "--control-port" && index + 1 < argc)
        {
            if (!ParseUint16(argv[++index], &options->control_port))
            {
                std::cerr << "[spectra_bridge_test] invalid --control-port" << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--image-port" && index + 1 < argc)
        {
            if (!ParseUint16(argv[++index], &options->image_port))
            {
                std::cerr << "[spectra_bridge_test] invalid --image-port" << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--image" && index + 1 < argc)
        {
            options->image_path = argv[++index];
            continue;
        }

        std::cerr << "[spectra_bridge_test] unknown argument: " << arg << std::endl;
        PrintUsage();
        return false;
    }

    return true;
}

bool FileExists(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return input.good();
}

std::vector<std::string> BuildImageSearchPaths(const std::string& configured_path)
{
    std::vector<std::string> paths;
    if (!configured_path.empty())
    {
        paths.push_back(configured_path);
    }
    return paths;
}

bool ReadPgmToken(std::istream& input, std::string* token)
{
    if (token == NULL)
    {
        return false;
    }

    token->clear();
    while (input.good())
    {
        int next = input.peek();
        if (next == '#')
        {
            std::string ignored;
            std::getline(input, ignored);
            continue;
        }
        if (std::isspace(next))
        {
            input.get();
            continue;
        }
        break;
    }

    return static_cast<bool>(input >> *token);
}

bool LoadPgm8(const std::string& path, std::vector<uint8_t>* image8, std::string* error)
{
    if (image8 == NULL)
    {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        if (error != NULL)
        {
            *error = "cannot open PGM image: " + path;
        }
        return false;
    }

    std::string magic;
    std::string width_text;
    std::string height_text;
    std::string max_value_text;

    if (!ReadPgmToken(input, &magic) ||
        !ReadPgmToken(input, &width_text) ||
        !ReadPgmToken(input, &height_text) ||
        !ReadPgmToken(input, &max_value_text))
    {
        if (error != NULL)
        {
            *error = "invalid PGM header";
        }
        return false;
    }

    if (magic != "P5")
    {
        if (error != NULL)
        {
            *error = "only binary PGM P5 is supported";
        }
        return false;
    }

    const int width = std::stoi(width_text);
    const int height = std::stoi(height_text);
    const int max_value = std::stoi(max_value_text);
    if (width != static_cast<int>(kImageWidth) ||
        height != static_cast<int>(kImageHeight) ||
        max_value != 255)
    {
        if (error != NULL)
        {
            std::ostringstream message;
            message << "PGM must be 800x600 maxval=255, actual "
                    << width << "x" << height << " maxval=" << max_value;
            *error = message.str();
        }
        return false;
    }

    // PGM header 后面会有一个空白字符；读掉它后就是 width*height 字节灰度数据。
    input.get();

    image8->assign(kImagePixelCount, 0u);
    input.read(reinterpret_cast<char*>(image8->data()), static_cast<std::streamsize>(image8->size()));
    if (input.gcount() != static_cast<std::streamsize>(image8->size()))
    {
        if (error != NULL)
        {
            *error = "PGM pixel data is shorter than expected";
        }
        return false;
    }

    return true;
}

std::vector<uint8_t> BuildFallbackSpectrumImage8()
{
    std::vector<uint8_t> image(kImagePixelCount, 8u);

    const int line_positions[] = {38, 148, 196, 257, 300, 343, 383, 492, 631, 657, 681, 718, 742};
    const int line_strengths[] = {50, 170, 110, 90, 140, 200, 150, 255, 245, 120, 95, 160, 80};

    for (uint32_t y = 0; y < kImageHeight; ++y)
    {
        for (uint32_t x = 0; x < kImageWidth; ++x)
        {
            int value = 10 + static_cast<int>((y * 8u) / kImageHeight);

            for (std::size_t line = 0; line < sizeof(line_positions) / sizeof(line_positions[0]); ++line)
            {
                int dx = static_cast<int>(x) - line_positions[line];
                int abs_dx = dx < 0 ? -dx : dx;
                if (abs_dx <= 14)
                {
                    int contribution = line_strengths[line] / (1 + abs_dx * abs_dx);
                    value += contribution;
                }
            }

            if (((x * 17u + y * 31u) % 997u) == 0u)
            {
                value = 220;
            }

            if (value > 255)
            {
                value = 255;
            }

            image[static_cast<std::size_t>(y) * kImageWidth + x] = static_cast<uint8_t>(value);
        }
    }

    return image;
}

uint32_t HashPixel(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t value = x * 374761393u + y * 668265263u + seed * 2246822519u;
    value ^= value >> 13u;
    value *= 1274126177u;
    value ^= value >> 16u;
    return value;
}

int ClampGray8(int value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return value;
}

int BroadLineContribution(uint32_t x, int center, int strength, int half_width)
{
    const int dx = static_cast<int>(x) - center;
    const int width2 = half_width * half_width;
    return (strength * width2) / (dx * dx + width2);
}

std::vector<uint8_t> BuildQualitySpectrumImage8(uint32_t seed, int variant)
{
    std::vector<uint8_t> image(kImagePixelCount, 0u);

    const int centers[6][6] = {
        {88, 176, 292, 421, 558, 698},
        {62, 205, 318, 462, 610, 735},
        {116, 244, 372, 496, 646, 724},
        {74, 188, 352, 510, 632, 758},
        {98, 226, 338, 455, 584, 710},
        {132, 264, 404, 536, 668, 748},
    };
    const int strengths[6][6] = {
        {42, 64, 50, 72, 58, 44},
        {36, 54, 68, 48, 62, 40},
        {52, 46, 74, 54, 42, 56},
        {44, 60, 42, 70, 48, 38},
        {40, 58, 52, 66, 46, 54},
        {34, 62, 44, 58, 72, 36},
    };
    const int widths[6][6] = {
        {18, 24, 22, 28, 24, 20},
        {22, 26, 30, 24, 28, 22},
        {20, 24, 32, 26, 24, 20},
        {24, 28, 22, 30, 26, 22},
        {18, 26, 24, 28, 24, 20},
        {24, 30, 22, 26, 32, 24},
    };

    const int profile = variant % 6;
    for (uint32_t y = 0; y < kImageHeight; ++y)
    {
        const int vertical_gradient = static_cast<int>((y * (8u + static_cast<uint32_t>(variant % 4))) / kImageHeight);
        const int soft_vignetting = static_cast<int>(((y > kImageHeight / 2u ? y - kImageHeight / 2u : kImageHeight / 2u - y) * 5u) /
                                                     (kImageHeight / 2u));

        for (uint32_t x = 0; x < kImageWidth; ++x)
        {
            int value = 28 + vertical_gradient - soft_vignetting;
            value += static_cast<int>((x * static_cast<uint32_t>(6 + (variant % 5))) / kImageWidth);

            for (int line = 0; line < 6; ++line)
            {
                value += BroadLineContribution(x,
                                               centers[profile][line],
                                               strengths[profile][line],
                                               widths[profile][line]);
            }

            // 轻微、连续的伪噪声只用于让测试图更像真实相机读数。
            // 振幅控制在很小范围内，避免触发坏点或异常行/列 FAIL。
            const uint32_t noise = HashPixel(x, y, seed) % 7u;
            value += static_cast<int>(noise) - 3;

            image[static_cast<std::size_t>(y) * kImageWidth + x] =
                static_cast<uint8_t>(ClampGray8(value));
        }
    }

    return image;
}

void AddMildHotPixels(std::vector<uint8_t>* image, uint32_t seed, int count)
{
    if (image == NULL)
    {
        return;
    }

    uint32_t state = seed == 0u ? 1u : seed;
    for (int index = 0; index < count; ++index)
    {
        state = state * 1664525u + 1013904223u;
        const uint32_t x = 1u + (state % (kImageWidth - 2u));
        state = state * 1664525u + 1013904223u;
        const uint32_t y = 1u + (state % (kImageHeight - 2u));
        const std::size_t pixel_index = static_cast<std::size_t>(y) * kImageWidth + x;
        (*image)[pixel_index] = 214u;
    }
}

void AddSaturationBlock(std::vector<uint8_t>* image, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
    if (image == NULL)
    {
        return;
    }

    const uint32_t right = std::min(left + width, kImageWidth);
    const uint32_t bottom = std::min(top + height, kImageHeight);
    for (uint32_t y = top; y < bottom; ++y)
    {
        for (uint32_t x = left; x < right; ++x)
        {
            (*image)[static_cast<std::size_t>(y) * kImageWidth + x] = 255u;
        }
    }
}

std::vector<TestImage> BuildGeneratedQualityImageBank()
{
    std::vector<TestImage> bank;
    bank.push_back({"PASS stable spectrum", BuildQualitySpectrumImage8(101u, 0)});

    TestImage warning;
    warning.name = "WARNING mild hot pixels";
    warning.pixels8 = BuildQualitySpectrumImage8(202u, 1);
    // 120 个离散热坏点预计超过后端 badPixelWarningCount=60，
    // 但低于 badPixelFailCount=500，用于稳定触发 WARNING。
    AddMildHotPixels(&warning.pixels8, 909u, 120);
    bank.push_back(warning);

    TestImage fail;
    fail.name = "FAIL saturated block";
    fail.pixels8 = BuildQualitySpectrumImage8(303u, 2);
    // 80 * 80 = 6400 个满量程像素，占 800*600 的 1.33%，
    // 高于后端 saturationFailRatio=1%，用于稳定触发 FAIL。
    AddSaturationBlock(&fail.pixels8, 360u, 120u, 80u, 80u);
    bank.push_back(fail);

    return bank;
}

std::vector<TestImage> LoadConfiguredImageBank(const ProgramOptions& options, std::string* loaded_description)
{
    if (options.image_path.empty())
    {
        if (loaded_description != NULL)
        {
            *loaded_description = "(generated PASS/WARNING/FAIL cyclic test image bank)";
        }
        return BuildGeneratedQualityImageBank();
    }

    std::vector<std::string> paths = BuildImageSearchPaths(options.image_path);
    for (const std::string& path : paths)
    {
        if (!FileExists(path))
        {
            continue;
        }

        std::vector<uint8_t> image8;
        std::string error;
        if (LoadPgm8(path, &image8, &error))
        {
            if (loaded_description != NULL)
            {
                *loaded_description = path;
            }
            return std::vector<TestImage>{{path, image8}};
        }

        std::cerr << "[spectra_bridge_test] failed to load "
                  << NormalizePathForLog(path) << ": " << error << std::endl;
    }

    if (loaded_description != NULL)
    {
        *loaded_description = "(generated fallback)";
    }
    std::cout << "[spectra_bridge_test] PGM image not found, using generated fallback spectrum" << std::endl;
    return std::vector<TestImage>{{"generated fallback", BuildFallbackSpectrumImage8()}};
}

std::vector<uint8_t> ConvertGray8ToRaw16Low12Payload(const std::vector<uint8_t>& image8)
{
    std::vector<uint8_t> payload(kImagePayloadSize, 0u);
    for (std::size_t pixel_index = 0; pixel_index < image8.size(); ++pixel_index)
    {
        // 8-bit 测试图只是人眼预览用灰度；模拟 FPGA 发送时扩展成 RAW12 DN。
        // 高 4 位必须为 0，否则 native 的 INVALID_HIGH_BITS 检查会正确拦截。
        const uint16_t pixel12 =
            static_cast<uint16_t>((static_cast<uint32_t>(image8[pixel_index]) * 4095u) / 255u);
        spectra::network::WriteUint16LE(pixel12, payload.data() + pixel_index * sizeof(uint16_t));
    }
    return payload;
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
    bits |= state.busy ? StatusBits::BUSY : StatusBits::READY;
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
    StatusSnapshot snapshot = MakeStatusSnapshot(state);

    std::array<uint8_t, spectra::protocol::kStatusPacketSize> packet = {};
    packet[0] = static_cast<uint8_t>(spectra::protocol::MsgType::ReturnStatus);
    spectra::network::WriteUint16LE(BuildStatusBits(snapshot), packet.data() + 1);
    spectra::network::WriteUint16LE(snapshot.error_code, packet.data() + 3);
    return SendAll(control_socket, packet.data(), packet.size());
}

bool SendConfigAckPacket(SOCKET control_socket, uint16_t result_code, uint16_t failed_addr)
{
    std::array<uint8_t, spectra::protocol::kConfigAckPacketSize> packet = {};
    packet[0] = static_cast<uint8_t>(spectra::protocol::MsgType::ConfigAck);
    spectra::network::WriteUint16LE(result_code, packet.data() + 1);
    spectra::network::WriteUint16LE(failed_addr, packet.data() + 3);
    return SendAll(control_socket, packet.data(), packet.size());
}

bool SendOneImageFrame(SOCKET image_socket, SharedState& state, const TestImage& image)
{
    std::vector<uint8_t> payload = ConvertGray8ToRaw16Low12Payload(image.pixels8);
    const uint32_t payload_crc32 = spectra::util::ComputeCrc32(payload.data(), payload.size());

    std::array<uint8_t, spectra::protocol::kImageFrameHeaderSize> header = {};
    spectra::network::WriteUint32LE(spectra::protocol::kImageMagic, header.data() + 0);
    spectra::network::WriteUint16LE(spectra::protocol::kImageProtocolVersion, header.data() + 4);
    spectra::network::WriteUint16LE(
        static_cast<uint16_t>(spectra::protocol::kImageFrameHeaderSize), header.data() + 6);
    spectra::network::WriteUint32LE(static_cast<uint32_t>(payload.size()), header.data() + 8);
    spectra::network::WriteUint32LE(kImageWidth, header.data() + 12);
    spectra::network::WriteUint32LE(kImageHeight, header.data() + 16);
    spectra::network::WriteUint32LE(spectra::protocol::kPixelFormatRaw16Low12, header.data() + 20);
    spectra::network::WriteUint32LE(payload_crc32, header.data() + 24);
    spectra::network::WriteUint32LE(0u, header.data() + 28);

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

    std::cout << "[spectra_bridge_test] image frame sent, name=\""
              << image.name << "\", payload="
              << payload.size() << " bytes, crc32=0x"
              << std::hex << std::uppercase << payload_crc32 << std::dec << std::endl;
    return true;
}

void ControlThreadMain(SOCKET control_socket, SharedState& state)
{
    while (true)
    {
        uint8_t msg_type = 0u;
        if (!RecvAll(control_socket, &msg_type, 1u))
        {
            std::cout << "[spectra_bridge_test] control client disconnected" << std::endl;
            return;
        }

        if (msg_type == static_cast<uint8_t>(spectra::protocol::MsgType::Control))
        {
            std::array<uint8_t, 2> body = {};
            if (!RecvAll(control_socket, body.data(), body.size()))
            {
                return;
            }

            const uint16_t control_bits = spectra::network::ReadUint16LE(body.data());
            {
                std::lock_guard<std::mutex> lock(state.mutex);

                if ((control_bits & spectra::protocol::ControlBits::RESET) != 0u)
                {
                    state.trigger_pending = false;
                    state.config_applied = false;
                    state.image_ready = false;
                    state.busy = false;
                    state.error_code = 0u;
                    std::cout << "[spectra_bridge_test] RESET received" << std::endl;
                }

                if ((control_bits & spectra::protocol::ControlBits::TRIGGER_ONCE) != 0u)
                {
                    state.trigger_pending = true;
                    state.image_ready = false;
                    state.busy = true;
                    std::cout << "[spectra_bridge_test] TRIGGER_ONCE received" << std::endl;
                }
            }

            state.image_cv.notify_one();

            StatusSnapshot snapshot;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                snapshot = MakeStatusSnapshot(state);
            }

            std::array<uint8_t, spectra::protocol::kStatusPacketSize> packet = {};
            packet[0] = static_cast<uint8_t>(spectra::protocol::MsgType::ReturnStatus);
            spectra::network::WriteUint16LE(BuildStatusBits(snapshot), packet.data() + 1);
            spectra::network::WriteUint16LE(snapshot.error_code, packet.data() + 3);
            if (!SendAll(control_socket, packet.data(), packet.size()))
            {
                return;
            }
            continue;
        }

        if (msg_type == static_cast<uint8_t>(spectra::protocol::MsgType::QueryStatus))
        {
            std::array<uint8_t, 2> body = {};
            if (!RecvAll(control_socket, body.data(), body.size()))
            {
                return;
            }

            std::cout << "[spectra_bridge_test] QUERY_STATUS received" << std::endl;
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!SendStatusPacket(control_socket, state))
            {
                return;
            }
            continue;
        }

        if (msg_type == static_cast<uint8_t>(spectra::protocol::MsgType::Config))
        {
            std::array<uint8_t, 2> payload_len_bytes = {};
            if (!RecvAll(control_socket, payload_len_bytes.data(), payload_len_bytes.size()))
            {
                return;
            }

            const uint16_t payload_len = spectra::network::ReadUint16LE(payload_len_bytes.data());
            std::vector<uint8_t> regs(payload_len, 0u);
            if (!RecvAll(control_socket, regs.data(), regs.size()))
            {
                return;
            }

            std::cout << "[spectra_bridge_test] CONFIG received, payload_len="
                      << payload_len << std::endl;

            const bool config_ok = (payload_len == spectra::protocol::kFullConfigPayloadSize);
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

        std::cout << "[spectra_bridge_test] unsupported msg_type="
                  << static_cast<int>(msg_type) << std::endl;
        return;
    }
}

void ImageThreadMain(SOCKET image_socket, SharedState& state, const std::vector<TestImage>& image_bank)
{
    std::size_t next_image_index = 0u;

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

        const TestImage& image = image_bank[next_image_index % image_bank.size()];
        ++next_image_index;

        std::cout << "[spectra_bridge_test] sending spectrum image: "
                  << image.name << " (" << next_image_index << ")" << std::endl;
        if (!SendOneImageFrame(image_socket, state, image))
        {
            std::cout << "[spectra_bridge_test] image client disconnected" << std::endl;
            return;
        }
    }
}

int RunSelfTest(const ProgramOptions& options)
{
    std::string loaded_description;
    std::vector<TestImage> image_bank = LoadConfiguredImageBank(options, &loaded_description);
    if (image_bank.empty())
    {
        std::cerr << "[spectra_bridge_test] self-test failed: image bank is empty" << std::endl;
        return 1;
    }

    std::string error;
    for (const TestImage& image : image_bank)
    {
        std::vector<uint8_t> payload = ConvertGray8ToRaw16Low12Payload(image.pixels8);

        std::array<uint8_t, spectra::protocol::kImageFrameHeaderSize> header = {};
        spectra::network::WriteUint32LE(spectra::protocol::kImageMagic, header.data() + 0);
        spectra::network::WriteUint16LE(spectra::protocol::kImageProtocolVersion, header.data() + 4);
        spectra::network::WriteUint16LE(
            static_cast<uint16_t>(spectra::protocol::kImageFrameHeaderSize), header.data() + 6);
        spectra::network::WriteUint32LE(static_cast<uint32_t>(payload.size()), header.data() + 8);
        spectra::network::WriteUint32LE(kImageWidth, header.data() + 12);
        spectra::network::WriteUint32LE(kImageHeight, header.data() + 16);
        spectra::network::WriteUint32LE(spectra::protocol::kPixelFormatRaw16Low12, header.data() + 20);
        spectra::network::WriteUint32LE(spectra::util::ComputeCrc32(payload.data(), payload.size()), header.data() + 24);

        spectra::protocol::ImageFrameHeader parsed = {};
        if (!spectra::protocol::ParseImageFrameHeader(header, &parsed, &error))
        {
            std::cerr << "[spectra_bridge_test] self-test failed for "
                      << image.name << ": " << error << std::endl;
            return 1;
        }
    }

    std::cout << "[spectra_bridge_test] self-test OK" << std::endl;
    std::cout << "  source=" << loaded_description << std::endl;
    std::cout << "  image_count=" << image_bank.size() << std::endl;
    std::cout << "  dimensions=" << kImageWidth << "x" << kImageHeight << std::endl;
    std::cout << "  payload=" << kImagePayloadSize << " bytes" << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    ProgramOptions options;
    if (!ParseOptions(argc, argv, &options))
    {
        return 1;
    }

    if (options.self_test)
    {
        return RunSelfTest(options);
    }

    std::string loaded_description;
    std::vector<TestImage> image_bank = LoadConfiguredImageBank(options, &loaded_description);
    if (image_bank.empty())
    {
        std::cerr << "[spectra_bridge_test] no image available" << std::endl;
        return 1;
    }
    std::cout << "[spectra_bridge_test] loaded image source: " << loaded_description
              << ", image_count=" << image_bank.size() << std::endl;
    std::cout << "[spectra_bridge_test] image protocol: 800x600 RAW16_LOW12 + CRC32" << std::endl;

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        std::cerr << "[spectra_bridge_test] WSAStartup failed" << std::endl;
        return 1;
    }

    SOCKET control_listen_socket = CreateListenSocket(options.control_port);
    SOCKET image_listen_socket = CreateListenSocket(options.image_port);
    if (control_listen_socket == INVALID_SOCKET || image_listen_socket == INVALID_SOCKET)
    {
        std::cerr << "[spectra_bridge_test] failed to create listen sockets. "
                  << "Are ports " << options.control_port << " and "
                  << options.image_port << " already in use?" << std::endl;

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

    std::cout << "[spectra_bridge_test] waiting for control connection on port "
              << options.control_port << std::endl;
    SOCKET control_socket = accept(control_listen_socket, NULL, NULL);
    if (control_socket == INVALID_SOCKET)
    {
        std::cerr << "[spectra_bridge_test] accept(control) failed" << std::endl;
        closesocket(control_listen_socket);
        closesocket(image_listen_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[spectra_bridge_test] control connected" << std::endl;

    std::cout << "[spectra_bridge_test] waiting for image connection on port "
              << options.image_port << std::endl;
    SOCKET image_socket = accept(image_listen_socket, NULL, NULL);
    if (image_socket == INVALID_SOCKET)
    {
        std::cerr << "[spectra_bridge_test] accept(image) failed" << std::endl;
        closesocket(control_socket);
        closesocket(control_listen_socket);
        closesocket(image_listen_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[spectra_bridge_test] image connected" << std::endl;

    SharedState state;
    std::thread control_thread(ControlThreadMain, control_socket, std::ref(state));
    std::thread image_thread(ImageThreadMain, image_socket, std::ref(state), std::cref(image_bank));

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

    std::cout << "[spectra_bridge_test] stopped" << std::endl;
    return 0;
}
