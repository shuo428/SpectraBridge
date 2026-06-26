#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "image/ImageConverter.h"
#include "network/ByteUtils.h"
#include "protocol/ImageProtocol.h"
#include "util/Crc32.h"

namespace {

using spectra::network::WriteUint16LE;
using spectra::network::WriteUint32LE;
using spectra::protocol::ImageFrameHeader;
using spectra::protocol::kImageFrameHeaderSize;
using spectra::protocol::kImageMagic;
using spectra::protocol::kImageProtocolVersion;
using spectra::protocol::kPixelFormatRaw16Low12;

struct TestRunner {
    int passed = 0;
    int failed = 0;

    void Run(const std::string& name, const std::function<void()>& test)
    {
        try
        {
            test();
            ++passed;
            std::cout << "[PASS] " << name << std::endl;
        }
        catch (const std::exception& ex)
        {
            ++failed;
            std::cout << "[FAIL] " << name << " - " << ex.what() << std::endl;
        }
    }
};

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireContains(const std::string& value, const std::string& expected)
{
    if (value.find(expected) == std::string::npos)
    {
        throw std::runtime_error("expected message to contain '" + expected + "', actual: " + value);
    }
}

std::array<uint8_t, kImageFrameHeaderSize> MakeHeader(uint32_t magic = kImageMagic,
                                                       uint16_t version = kImageProtocolVersion,
                                                       uint16_t header_len = static_cast<uint16_t>(kImageFrameHeaderSize),
                                                       uint32_t payload_len = 8u,
                                                       uint32_t width = 2u,
                                                       uint32_t height = 2u,
                                                       uint32_t pixel_format = kPixelFormatRaw16Low12,
                                                       uint32_t crc32 = 0u,
                                                       uint32_t reserved = 0u)
{
    std::array<uint8_t, kImageFrameHeaderSize> header = {};
    WriteUint32LE(magic, header.data());
    WriteUint16LE(version, header.data() + 4);
    WriteUint16LE(header_len, header.data() + 6);
    WriteUint32LE(payload_len, header.data() + 8);
    WriteUint32LE(width, header.data() + 12);
    WriteUint32LE(height, header.data() + 16);
    WriteUint32LE(pixel_format, header.data() + 20);
    WriteUint32LE(crc32, header.data() + 24);
    WriteUint32LE(reserved, header.data() + 28);
    return header;
}

bool ParseHeader(const std::array<uint8_t, kImageFrameHeaderSize>& wire_header,
                 ImageFrameHeader* parsed,
                 std::string* error)
{
    return spectra::protocol::ParseImageFrameHeader(wire_header, parsed, error);
}

std::vector<uint8_t> MakeRaw16Payload(const std::vector<uint16_t>& pixels)
{
    std::vector<uint8_t> payload(pixels.size() * sizeof(uint16_t), 0u);
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        WriteUint16LE(pixels[index], payload.data() + index * sizeof(uint16_t));
    }
    return payload;
}

void ExpectHeaderRejected(const std::array<uint8_t, kImageFrameHeaderSize>& wire_header,
                          const std::string& expected_code)
{
    ImageFrameHeader parsed = {};
    std::string error;
    Require(!ParseHeader(wire_header, &parsed, &error), "header should be rejected");
    RequireContains(error, expected_code);
}

}  // namespace

int main()
{
    TestRunner runner;

    runner.Run("valid image header is accepted", [] {
        ImageFrameHeader parsed = {};
        std::string error;
        Require(ParseHeader(MakeHeader(), &parsed, &error), "valid header rejected: " + error);
        Require(parsed.magic == kImageMagic, "magic not parsed");
        Require(parsed.version == kImageProtocolVersion, "version not parsed");
        Require(parsed.header_len == kImageFrameHeaderSize, "header_len not parsed");
        Require(parsed.payload_len == 8u, "payload_len not parsed");
        Require(parsed.width == 2u && parsed.height == 2u, "dimensions not parsed");
        Require(parsed.pixel_format == kPixelFormatRaw16Low12, "pixel_format not parsed");
    });

    runner.Run("invalid magic is rejected", [] {
        ExpectHeaderRejected(MakeHeader(0x12345678u), "[INVALID_MAGIC]");
    });

    runner.Run("unsupported protocol version is rejected", [] {
        ExpectHeaderRejected(MakeHeader(kImageMagic, 2u), "[UNSUPPORTED_VERSION]");
    });

    runner.Run("invalid header length is rejected", [] {
        ExpectHeaderRejected(MakeHeader(kImageMagic,
                                        kImageProtocolVersion,
                                        static_cast<uint16_t>(kImageFrameHeaderSize - 1u)),
                             "[INVALID_HEADER_LENGTH]");
        ExpectHeaderRejected(MakeHeader(kImageMagic,
                                        kImageProtocolVersion,
                                        257u),
                             "[INVALID_HEADER_LENGTH]");
    });

    runner.Run("zero dimensions are rejected", [] {
        ExpectHeaderRejected(MakeHeader(kImageMagic,
                                        kImageProtocolVersion,
                                        static_cast<uint16_t>(kImageFrameHeaderSize),
                                        0u,
                                        0u,
                                        2u),
                             "[INVALID_DIMENSIONS]");
    });

    runner.Run("invalid pixel format is rejected", [] {
        ExpectHeaderRejected(MakeHeader(kImageMagic,
                                        kImageProtocolVersion,
                                        static_cast<uint16_t>(kImageFrameHeaderSize),
                                        8u,
                                        2u,
                                        2u,
                                        0x99999999u),
                             "[INVALID_PIXEL_FORMAT]");
    });

    runner.Run("reserved field must be zero", [] {
        ExpectHeaderRejected(MakeHeader(kImageMagic,
                                        kImageProtocolVersion,
                                        static_cast<uint16_t>(kImageFrameHeaderSize),
                                        8u,
                                        2u,
                                        2u,
                                        kPixelFormatRaw16Low12,
                                        0u,
                                        1u),
                             "[INVALID_RESERVED_FIELD]");
    });

    runner.Run("payload length must equal width * height * 2", [] {
        ExpectHeaderRejected(MakeHeader(kImageMagic,
                                        kImageProtocolVersion,
                                        static_cast<uint16_t>(kImageFrameHeaderSize),
                                        6u,
                                        2u,
                                        2u),
                             "[PAYLOAD_LENGTH_MISMATCH]");
    });

    runner.Run("payload size calculation overflow is rejected", [] {
        ExpectHeaderRejected(MakeHeader(kImageMagic,
                                        kImageProtocolVersion,
                                        static_cast<uint16_t>(kImageFrameHeaderSize),
                                        8u,
                                        0xFFFFFFFFu,
                                        0xFFFFFFFFu),
                             "[INVALID_DIMENSIONS]");
    });

    runner.Run("RAW16 low12 payload converts to 16-bit and 8-bit buffers", [] {
        const std::vector<uint8_t> payload = MakeRaw16Payload({0u, 17u, 2048u, 4095u});
        spectra::image::ConvertedImageFrame frame = {};
        std::string error;
        Require(spectra::image::ConvertRaw16Low12ToGray(payload, 2u, 2u, &frame, &error),
                "valid payload rejected: " + error);
        Require(frame.width == 2u && frame.height == 2u, "converted dimensions are wrong");
        Require(frame.pixels16.size() == 4u && frame.pixels8.size() == 4u, "converted buffer sizes are wrong");
        Require(frame.pixels16[0] == 0u, "pixel16[0] wrong");
        Require(frame.pixels16[1] == 17u, "pixel16[1] wrong");
        Require(frame.pixels16[2] == 2048u, "pixel16[2] wrong");
        Require(frame.pixels16[3] == 4095u, "pixel16[3] wrong");
        Require(frame.pixels8[0] == 0u, "pixel8[0] wrong");
        Require(frame.pixels8[3] == 255u, "pixel8[3] wrong");
    });

    runner.Run("RAW16 high bits are rejected before masking", [] {
        const std::vector<uint8_t> payload = MakeRaw16Payload({0x1000u, 0u, 0u, 0u});
        spectra::image::ConvertedImageFrame frame = {};
        std::string error;
        Require(!spectra::image::ConvertRaw16Low12ToGray(payload, 2u, 2u, &frame, &error),
                "payload with non-zero high bits should be rejected");
        RequireContains(error, "[INVALID_HIGH_BITS]");
    });

    runner.Run("RAW16 payload size mismatch is rejected", [] {
        const std::vector<uint8_t> payload = MakeRaw16Payload({0u, 1u, 2u});
        spectra::image::ConvertedImageFrame frame = {};
        std::string error;
        Require(!spectra::image::ConvertRaw16Low12ToGray(payload, 2u, 2u, &frame, &error),
                "short payload should be rejected");
        RequireContains(error, "[PAYLOAD_LENGTH_MISMATCH]");
    });

    runner.Run("CRC32 implementation matches standard check value", [] {
        const char* text = "123456789";
        const uint32_t crc = spectra::util::ComputeCrc32(
            reinterpret_cast<const uint8_t*>(text),
            9u);
        Require(crc == 0xCBF43926u, "CRC32 check value mismatch");
    });

    std::cout << std::endl
              << "SpectraBridge integrity tests: "
              << runner.passed << " passed, "
              << runner.failed << " failed" << std::endl;

    return runner.failed == 0 ? 0 : 1;
}
