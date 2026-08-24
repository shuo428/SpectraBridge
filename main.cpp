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
#include "image/ImageConverter.h"
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
    std::string fixture_dir;
    std::string scene = "normal";
    bool self_test = false;
    bool hdr_mode = false;
};

struct TestImage {
    std::string name;
    std::vector<uint8_t> pixels8;
    std::vector<uint8_t> hg_pixels8;
    std::vector<uint8_t> lg_pixels8;
};

enum class FixtureScene {
    kNormal,
    kHdr,
    kDark,
    kFlat,
    kHdrDark,
    kHdrFlat
};

struct FixtureSpec {
    std::string name;
    std::string image_path;
    std::string hg_path;
    std::string lg_path;
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

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ParseFixtureScene(const std::string& scene_text, FixtureScene* scene)
{
    if (scene == NULL)
    {
        return false;
    }

    const std::string normalized = ToLowerAscii(scene_text);
    if (normalized == "normal" || normalized == "single" || normalized == "spectrum")
    {
        *scene = FixtureScene::kNormal;
        return true;
    }
    if (normalized == "hdr")
    {
        *scene = FixtureScene::kHdr;
        return true;
    }
    if (normalized == "dark")
    {
        *scene = FixtureScene::kDark;
        return true;
    }
    if (normalized == "flat")
    {
        *scene = FixtureScene::kFlat;
        return true;
    }
    if (normalized == "hdr-dark" || normalized == "hdr_dark")
    {
        *scene = FixtureScene::kHdrDark;
        return true;
    }
    if (normalized == "hdr-flat" || normalized == "hdr_flat")
    {
        *scene = FixtureScene::kHdrFlat;
        return true;
    }

    return false;
}

bool SceneUsesHdrPayload(FixtureScene scene)
{
    return scene == FixtureScene::kHdr ||
           scene == FixtureScene::kHdrDark ||
           scene == FixtureScene::kHdrFlat;
}

const char* SceneName(FixtureScene scene)
{
    switch (scene)
    {
    case FixtureScene::kNormal:
        return "normal";
    case FixtureScene::kHdr:
        return "hdr";
    case FixtureScene::kDark:
        return "dark";
    case FixtureScene::kFlat:
        return "flat";
    case FixtureScene::kHdrDark:
        return "hdr-dark";
    case FixtureScene::kHdrFlat:
        return "hdr-flat";
    default:
        return "normal";
    }
}

void PrintUsage()
{
    std::cout
        << "Usage:\n"
        << "  spectra_bridge_test.exe [--control-port 5000] [--image-port 5001]\n"
        << "                          [--scene normal|hdr|dark|flat|hdr-dark|hdr-flat]\n"
        << "                          [--fixture-dir tools/mock-fpga/test-fixtures]\n"
        << "                          [--image path.pgm] [--hdr]\n"
        << "  spectra_bridge_test.exe --self-test\n\n"
        << "If --image is omitted, the program first loads the generated fixture bank\n"
        << "for the selected scene. If fixtures are missing, it falls back to an\n"
        << "in-memory synthetic image bank.\n"
        << "--hdr is kept for compatibility; HDR scenes automatically send HG followed by LG.\n";
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
        if (arg == "--hdr")
        {
            options->hdr_mode = true;
            continue;
        }
        if (arg == "--scene" && index + 1 < argc)
        {
            options->scene = argv[++index];
            continue;
        }
        if (arg == "--fixture-dir" && index + 1 < argc)
        {
            options->fixture_dir = argv[++index];
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

    FixtureScene parsed_scene = FixtureScene::kNormal;
    if (!ParseFixtureScene(options->scene, &parsed_scene))
    {
        std::cerr << "[spectra_bridge_test] invalid --scene: " << options->scene << std::endl;
        PrintUsage();
        return false;
    }
    options->scene = SceneName(parsed_scene);
    if (SceneUsesHdrPayload(parsed_scene))
    {
        options->hdr_mode = true;
    }
    else if (options->hdr_mode && options->scene == "normal")
    {
        options->scene = "hdr";
    }
    else if (options->hdr_mode)
    {
        std::cerr << "[spectra_bridge_test] --hdr can only be combined with --scene normal. "
                  << "Use --scene hdr-dark or --scene hdr-flat for HDR calibration fixtures." << std::endl;
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

std::string JoinPath(const std::string& base, const std::string& relative)
{
    if (base.empty())
    {
        return relative;
    }
    const char last = base[base.size() - 1u];
    if (last == '/' || last == '\\')
    {
        return base + relative;
    }
    return base + "/" + relative;
}

void AddUniquePath(std::vector<std::string>* paths, const std::string& path)
{
    if (paths == NULL || path.empty())
    {
        return;
    }
    if (std::find(paths->begin(), paths->end(), path) == paths->end())
    {
        paths->push_back(path);
    }
}

std::vector<std::string> BuildFixtureSearchDirs(const std::string& configured_dir)
{
    std::vector<std::string> dirs;
    AddUniquePath(&dirs, configured_dir);
    AddUniquePath(&dirs, "tools/mock-fpga/test-fixtures");
    AddUniquePath(&dirs, "../tools/mock-fpga/test-fixtures");
    AddUniquePath(&dirs, "../../tools/mock-fpga/test-fixtures");
    AddUniquePath(&dirs, "SpectraBridge/tools/mock-fpga/test-fixtures");
    return dirs;
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

std::vector<uint8_t> ScaleImage8(const std::vector<uint8_t>& image8,
                                 uint32_t numerator,
                                 uint32_t denominator,
                                 int offset = 0)
{
    if (denominator == 0u)
    {
        denominator = 1u;
    }
    std::vector<uint8_t> scaled(image8.size(), 0u);
    for (std::size_t index = 0u; index < image8.size(); ++index)
    {
        const int value =
            static_cast<int>((static_cast<uint32_t>(image8[index]) * numerator) / denominator) + offset;
        scaled[index] = static_cast<uint8_t>(ClampGray8(value));
    }
    return scaled;
}

std::vector<uint8_t> BuildGeneratedCalibrationImage8(uint32_t seed,
                                                     int base,
                                                     int amplitude,
                                                     bool flat_like)
{
    std::vector<uint8_t> image(kImagePixelCount, 0u);
    for (uint32_t y = 0; y < kImageHeight; ++y)
    {
        for (uint32_t x = 0; x < kImageWidth; ++x)
        {
            int value = base;
            if (flat_like)
            {
                const int dx = static_cast<int>(x) - static_cast<int>(kImageWidth / 2u);
                const int dy = static_cast<int>(y) - static_cast<int>(kImageHeight / 2u);
                const int radial = (dx * dx) / 19000 + (dy * dy) / 14000;
                value += amplitude - radial;
                value += static_cast<int>((x * 8u) / kImageWidth);
            }
            else
            {
                value += static_cast<int>((y * 3u) / kImageHeight);
            }

            const uint32_t noise = HashPixel(x, y, seed) % 5u;
            value += static_cast<int>(noise) - 2;
            image[static_cast<std::size_t>(y) * kImageWidth + x] =
                static_cast<uint8_t>(ClampGray8(value));
        }
    }

    // 固定缺陷点和一条轻微异常行用于测试校准包生成后的稳定缺陷地图。
    const uint32_t defect_seed = seed + 0x5A17u;
    AddMildHotPixels(&image, defect_seed, 30);
    const uint32_t row = 120u + (seed % 180u);
    for (uint32_t x = 0u; x < kImageWidth; ++x)
    {
        const std::size_t index = static_cast<std::size_t>(row) * kImageWidth + x;
        image[index] = static_cast<uint8_t>(ClampGray8(static_cast<int>(image[index]) + (flat_like ? -24 : 18)));
    }

    return image;
}

std::vector<TestImage> BuildGeneratedHdrImageBank()
{
    std::vector<TestImage> bank;
    for (const TestImage& normal : BuildGeneratedQualityImageBank())
    {
        TestImage hdr;
        hdr.name = "HDR " + normal.name;
        hdr.pixels8 = normal.pixels8;
        hdr.hg_pixels8 = normal.pixels8;
        hdr.lg_pixels8 = ScaleImage8(normal.pixels8, 1u, 4u, 4);
        bank.push_back(hdr);
    }

    if (bank.size() >= 3u)
    {
        // HDR FAIL 需要 HG/LG 在同一块区域同时不可用，否则融合会用 LG 接管 HG 饱和区域。
        AddSaturationBlock(&bank[2].lg_pixels8, 360u, 120u, 80u, 80u);
    }
    return bank;
}

std::vector<TestImage> BuildGeneratedSingleCalibrationBank(const std::string& label,
                                                           bool flat_like)
{
    std::vector<TestImage> bank;
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        TestImage image;
        std::ostringstream name;
        name << label << " synthetic sample " << (index + 1u);
        image.name = name.str();
        image.pixels8 = BuildGeneratedCalibrationImage8(
            700u + index * 17u + (flat_like ? 2000u : 1000u),
            flat_like ? 132 : 7,
            flat_like ? 42 : 0,
            flat_like);
        bank.push_back(image);
    }
    return bank;
}

std::vector<TestImage> BuildGeneratedHdrCalibrationBank(const std::string& label,
                                                        bool flat_like)
{
    std::vector<TestImage> bank;
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        TestImage image;
        std::ostringstream name;
        name << label << " synthetic HG/LG sample " << (index + 1u);
        image.name = name.str();
        image.hg_pixels8 = BuildGeneratedCalibrationImage8(
            3000u + index * 19u + (flat_like ? 1000u : 0u),
            flat_like ? 145 : 8,
            flat_like ? 48 : 0,
            flat_like);
        image.lg_pixels8 = BuildGeneratedCalibrationImage8(
            4000u + index * 23u + (flat_like ? 1000u : 0u),
            flat_like ? 48 : 3,
            flat_like ? 18 : 0,
            flat_like);
        image.pixels8 = image.hg_pixels8;
        bank.push_back(image);
    }
    return bank;
}

std::vector<TestImage> BuildGeneratedImageBankForScene(FixtureScene scene)
{
    if (scene == FixtureScene::kHdr)
    {
        return BuildGeneratedHdrImageBank();
    }
    if (scene == FixtureScene::kDark)
    {
        return BuildGeneratedSingleCalibrationBank("DARK", false);
    }
    if (scene == FixtureScene::kFlat)
    {
        return BuildGeneratedSingleCalibrationBank("FLAT", true);
    }
    if (scene == FixtureScene::kHdrDark)
    {
        return BuildGeneratedHdrCalibrationBank("HDR_DARK", false);
    }
    if (scene == FixtureScene::kHdrFlat)
    {
        return BuildGeneratedHdrCalibrationBank("HDR_FLAT", true);
    }
    return BuildGeneratedQualityImageBank();
}

std::vector<FixtureSpec> BuildFixtureSpecs(FixtureScene scene)
{
    std::vector<FixtureSpec> specs;
    if (scene == FixtureScene::kNormal)
    {
        specs.push_back({"NORMAL PASS", "normal/normal_pass.pgm", "", ""});
        specs.push_back({"NORMAL WARNING", "normal/normal_warning_hot_pixels.pgm", "", ""});
        specs.push_back({"NORMAL FAIL", "normal/normal_fail_saturation.pgm", "", ""});
        return specs;
    }
    if (scene == FixtureScene::kHdr)
    {
        specs.push_back({"HDR PASS", "", "hdr/hdr_pass_hg.pgm", "hdr/hdr_pass_lg.pgm"});
        specs.push_back({"HDR WARNING", "", "hdr/hdr_warning_hg.pgm", "hdr/hdr_warning_lg.pgm"});
        specs.push_back({"HDR FAIL", "", "hdr/hdr_fail_hg.pgm", "hdr/hdr_fail_lg.pgm"});
        return specs;
    }

    const bool hdr = SceneUsesHdrPayload(scene);
    const std::string folder =
        scene == FixtureScene::kDark ? "dark" :
        scene == FixtureScene::kFlat ? "flat" :
        scene == FixtureScene::kHdrDark ? "hdr_dark" : "hdr_flat";
    const std::string prefix =
        scene == FixtureScene::kDark ? "dark" :
        scene == FixtureScene::kFlat ? "flat" :
        scene == FixtureScene::kHdrDark ? "hdr_dark" : "hdr_flat";
    const std::string label =
        scene == FixtureScene::kDark ? "DARK" :
        scene == FixtureScene::kFlat ? "FLAT" :
        scene == FixtureScene::kHdrDark ? "HDR_DARK" : "HDR_FLAT";

    for (int index = 1; index <= 8; ++index)
    {
        std::ostringstream two_digits;
        if (index < 10)
        {
            two_digits << "0";
        }
        two_digits << index;
        const std::string frame = two_digits.str();
        std::ostringstream name;
        name << label << " sample " << frame;
        if (hdr)
        {
            specs.push_back({
                name.str(),
                "",
                folder + "/" + prefix + "_" + frame + "_hg.pgm",
                folder + "/" + prefix + "_" + frame + "_lg.pgm",
            });
        }
        else
        {
            specs.push_back({
                name.str(),
                folder + "/" + prefix + "_" + frame + ".pgm",
                "",
                "",
            });
        }
    }
    return specs;
}

bool LoadFixtureBankFromDir(const std::string& dir,
                            FixtureScene scene,
                            std::vector<TestImage>* bank,
                            std::string* error)
{
    if (bank == NULL)
    {
        return false;
    }

    bank->clear();
    const std::vector<FixtureSpec> specs = BuildFixtureSpecs(scene);
    for (const FixtureSpec& spec : specs)
    {
        TestImage image;
        image.name = spec.name;
        if (SceneUsesHdrPayload(scene))
        {
            const std::string hg_path = JoinPath(dir, spec.hg_path);
            const std::string lg_path = JoinPath(dir, spec.lg_path);
            if (!FileExists(hg_path) || !FileExists(lg_path))
            {
                if (error != NULL)
                {
                    *error = "missing HDR fixture pair: " + hg_path + " / " + lg_path;
                }
                bank->clear();
                return false;
            }
            std::string load_error;
            if (!LoadPgm8(hg_path, &image.hg_pixels8, &load_error) ||
                !LoadPgm8(lg_path, &image.lg_pixels8, &load_error))
            {
                if (error != NULL)
                {
                    *error = load_error;
                }
                bank->clear();
                return false;
            }
            image.pixels8 = image.hg_pixels8;
        }
        else
        {
            const std::string path = JoinPath(dir, spec.image_path);
            if (!FileExists(path))
            {
                if (error != NULL)
                {
                    *error = "missing fixture: " + path;
                }
                bank->clear();
                return false;
            }
            std::string load_error;
            if (!LoadPgm8(path, &image.pixels8, &load_error))
            {
                if (error != NULL)
                {
                    *error = load_error;
                }
                bank->clear();
                return false;
            }
        }
        bank->push_back(image);
    }

    return !bank->empty();
}

std::vector<TestImage> LoadFixtureImageBank(const ProgramOptions& options,
                                            FixtureScene scene,
                                            std::string* loaded_description)
{
    const std::vector<std::string> dirs = BuildFixtureSearchDirs(options.fixture_dir);
    for (const std::string& dir : dirs)
    {
        std::vector<TestImage> bank;
        std::string error;
        if (LoadFixtureBankFromDir(dir, scene, &bank, &error))
        {
            if (loaded_description != NULL)
            {
                std::ostringstream description;
                description << dir << " (scene=" << SceneName(scene) << ")";
                *loaded_description = description.str();
            }
            return bank;
        }
    }

    return {};
}

std::vector<TestImage> LoadConfiguredImageBank(const ProgramOptions& options, std::string* loaded_description)
{
    FixtureScene scene = FixtureScene::kNormal;
    ParseFixtureScene(options.scene, &scene);

    if (options.image_path.empty())
    {
        std::vector<TestImage> fixture_bank = LoadFixtureImageBank(options, scene, loaded_description);
        if (!fixture_bank.empty())
        {
            return fixture_bank;
        }

        if (loaded_description != NULL)
        {
            std::ostringstream description;
            description << "(generated in-memory scene=" << SceneName(scene) << " fallback)";
            *loaded_description = description.str();
        }
        return BuildGeneratedImageBankForScene(scene);
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
            TestImage image;
            image.name = path;
            image.pixels8 = image8;
            if (options.hdr_mode)
            {
                image.hg_pixels8 = image8;
                image.lg_pixels8 = ScaleImage8(image8, 1u, 4u, 4);
            }
            return std::vector<TestImage>{image};
        }

        std::cerr << "[spectra_bridge_test] failed to load "
                  << NormalizePathForLog(path) << ": " << error << std::endl;
    }

    if (loaded_description != NULL)
    {
        std::ostringstream description;
        description << "(generated in-memory scene=" << SceneName(scene) << " fallback)";
        *loaded_description = description.str();
    }
    std::cout << "[spectra_bridge_test] PGM image not found, using generated fixture fallback" << std::endl;
    return BuildGeneratedImageBankForScene(scene);
}

std::vector<uint8_t> ConvertGray8ToRaw16Low12Payload(const std::vector<uint8_t>& image8,
                                                     uint32_t numerator = 1u,
                                                     uint32_t denominator = 1u)
{
    std::vector<uint8_t> payload(kImagePayloadSize, 0u);
    constexpr uint32_t kLaneCount = 4u;
    const uint32_t lane_width = kImageWidth / kLaneCount;
    if (denominator == 0u)
    {
        denominator = 1u;
    }
    for (uint32_t y = 0; y < kImageHeight; ++y)
    {
        for (uint32_t sample = 0; sample < lane_width; ++sample)
        {
            for (uint32_t lane = 0; lane < kLaneCount; ++lane)
            {
                // 模拟 Figure 42 中纯有效像素的 4-lane 交织顺序：
                // 0, laneWidth, 2*laneWidth, 3*laneWidth, 1, laneWidth+1...
                const uint32_t x = lane * lane_width + sample;
                const std::size_t row_major_index = static_cast<std::size_t>(y) * kImageWidth + x;
                const std::size_t payload_pixel_index =
                    static_cast<std::size_t>(y) * kImageWidth + sample * kLaneCount + lane;

                // 8-bit 测试图只是人眼预览用灰度；模拟 FPGA 发送时扩展成 RAW12 DN。
                // 高 4 位必须为 0，否则 native 的 INVALID_HIGH_BITS 检查会正确拦截。
                const uint32_t raw12 =
                    (static_cast<uint32_t>(image8[row_major_index]) * 4095u) / 255u;
                const uint32_t scaled = (raw12 * numerator) / denominator;
                const uint16_t pixel12 =
                    static_cast<uint16_t>(scaled > 4095u ? 4095u : scaled);
                spectra::network::WriteUint16LE(
                    pixel12, payload.data() + payload_pixel_index * sizeof(uint16_t));
            }
        }
    }
    return payload;
}

std::vector<uint8_t> ConvertGray8ToHdrRaw16Low12Payload(const std::vector<uint8_t>& image8)
{
    std::vector<uint8_t> payload = ConvertGray8ToRaw16Low12Payload(image8);
    std::vector<uint8_t> lg_payload = ConvertGray8ToRaw16Low12Payload(image8, 1u, 4u);
    payload.insert(payload.end(), lg_payload.begin(), lg_payload.end());
    return payload;
}

std::vector<uint8_t> ConvertGray8PairToHdrRaw16Low12Payload(const std::vector<uint8_t>& hg_image8,
                                                            const std::vector<uint8_t>& lg_image8)
{
    std::vector<uint8_t> payload = ConvertGray8ToRaw16Low12Payload(hg_image8);
    std::vector<uint8_t> lg_payload = ConvertGray8ToRaw16Low12Payload(lg_image8);
    payload.insert(payload.end(), lg_payload.begin(), lg_payload.end());
    return payload;
}

std::vector<uint8_t> BuildHdrPayloadForImage(const TestImage& image)
{
    const std::vector<uint8_t>& hg = image.hg_pixels8.empty() ? image.pixels8 : image.hg_pixels8;
    if (image.lg_pixels8.empty())
    {
        return ConvertGray8ToHdrRaw16Low12Payload(hg);
    }
    return ConvertGray8PairToHdrRaw16Low12Payload(hg, image.lg_pixels8);
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

bool SendOneImageFrame(SOCKET image_socket, SharedState& state, const TestImage& image, bool hdr_mode)
{
    std::vector<uint8_t> payload = hdr_mode
        ? BuildHdrPayloadForImage(image)
        : ConvertGray8ToRaw16Low12Payload(image.pixels8);
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

    std::cout << "[spectra_bridge_test] "
              << (hdr_mode ? "HDR HG+LG image frame" : "image frame")
              << " sent, name=\""
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

void ImageThreadMain(SOCKET image_socket,
                     SharedState& state,
                     const std::vector<TestImage>& image_bank,
                     bool hdr_mode)
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

        std::cout << "[spectra_bridge_test] sending "
                  << (hdr_mode ? "HDR HG+LG spectrum image: " : "spectrum image: ")
                  << image.name << " (" << next_image_index << ")" << std::endl;
        if (!SendOneImageFrame(image_socket, state, image, hdr_mode))
        {
            std::cout << "[spectra_bridge_test] image client disconnected" << std::endl;
            return;
        }
    }
}

uint16_t ExpectedRaw12FromGray8(uint8_t value)
{
    return static_cast<uint16_t>((static_cast<uint32_t>(value) * 4095u) / 255u);
}

bool VerifyConvertedPlane(const std::vector<uint16_t>& actual_pixels16,
                          const std::vector<uint8_t>& expected_pixels8,
                          const std::string& image_name,
                          const std::string& plane_name)
{
    if (actual_pixels16.size() != expected_pixels8.size())
    {
        std::cerr << "[spectra_bridge_test] self-test size mismatch for "
                  << image_name << " " << plane_name
                  << ", expected_pixels=" << expected_pixels8.size()
                  << ", actual_pixels=" << actual_pixels16.size() << std::endl;
        return false;
    }

    for (std::size_t pixel_index = 0u; pixel_index < expected_pixels8.size(); ++pixel_index)
    {
        const uint16_t expected12 = ExpectedRaw12FromGray8(expected_pixels8[pixel_index]);
        if (actual_pixels16[pixel_index] != expected12)
        {
            std::cerr << "[spectra_bridge_test] GLUX reorder self-test mismatch for "
                      << image_name << " " << plane_name
                      << " at pixel_index=" << pixel_index
                      << ", expected=" << expected12
                      << ", actual=" << actual_pixels16[pixel_index] << std::endl;
            return false;
        }
    }
    return true;
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
    const bool hdr_payload_mode = options.hdr_mode;
    for (const TestImage& image : image_bank)
    {
        std::vector<uint8_t> payload = hdr_payload_mode
            ? BuildHdrPayloadForImage(image)
            : ConvertGray8ToRaw16Low12Payload(image.pixels8);

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

        if (hdr_payload_mode)
        {
            spectra::image::ConvertedHdrImageFrame converted = {};
            if (!spectra::image::ConvertHdrRaw16Low12ToPlanes(
                    payload,
                    kImageWidth,
                    kImageHeight,
                    spectra::image::ReadoutOrder::kGlux1605Hdr4LaneInterleaved,
                    &converted,
                    &error))
            {
                std::cerr << "[spectra_bridge_test] HDR GLUX reorder self-test failed for "
                          << image.name << ": " << error << std::endl;
                return 1;
            }

            const std::vector<uint8_t>& expected_hg =
                image.hg_pixels8.empty() ? image.pixels8 : image.hg_pixels8;
            const std::vector<uint8_t>& expected_lg =
                image.lg_pixels8.empty() ? ScaleImage8(expected_hg, 1u, 4u) : image.lg_pixels8;
            if (!VerifyConvertedPlane(converted.hg_pixels16, expected_hg, image.name, "HG") ||
                !VerifyConvertedPlane(converted.lg_pixels16, expected_lg, image.name, "LG"))
            {
                return 1;
            }
        }
        else
        {
            spectra::image::ConvertedImageFrame converted = {};
            if (!spectra::image::ConvertRaw16Low12ToGray(
                    payload,
                    kImageWidth,
                    kImageHeight,
                    spectra::image::ReadoutOrder::kGlux1605Hdr4LaneInterleaved,
                    &converted,
                    &error))
            {
                std::cerr << "[spectra_bridge_test] GLUX reorder self-test failed for "
                          << image.name << ": " << error << std::endl;
                return 1;
            }

            if (!VerifyConvertedPlane(converted.pixels16, image.pixels8, image.name, "single"))
            {
                return 1;
            }
        }
    }

    std::cout << "[spectra_bridge_test] self-test OK" << std::endl;
    std::cout << "  source=" << loaded_description << std::endl;
    std::cout << "  image_count=" << image_bank.size() << std::endl;
    std::cout << "  scene=" << options.scene << std::endl;
    std::cout << "  payload_mode=" << (hdr_payload_mode ? "HDR_HG_THEN_LG" : "SINGLE_PLANE") << std::endl;
    std::cout << "  dimensions=" << kImageWidth << "x" << kImageHeight << std::endl;
    std::cout << "  payload=" << (hdr_payload_mode ? kImagePayloadSize * 2u : kImagePayloadSize)
              << " bytes" << std::endl;
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
    std::cout << "[spectra_bridge_test] image protocol: 800x600 RAW16_LOW12 + CRC32, "
              << "payload order=GLUX1605 HDR 4-lane interleaved effective pixels" << std::endl;
    std::cout << "[spectra_bridge_test] fixture scene: " << options.scene << std::endl;
    std::cout << "[spectra_bridge_test] capture payload mode: "
              << (options.hdr_mode ? "HDR HG full frame followed by LG full frame" : "single plane")
              << std::endl;

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
    std::thread image_thread(
        ImageThreadMain,
        image_socket,
        std::ref(state),
        std::cref(image_bank),
        options.hdr_mode);

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
