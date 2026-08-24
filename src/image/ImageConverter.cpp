#include "image/ImageConverter.h"

#include <cstddef>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#include "network/ByteUtils.h"
#include "protocol/ImageProtocol.h"

namespace spectra {
namespace image {

namespace {

const char* ReadoutOrderName(ReadoutOrder readout_order)
{
    switch (readout_order)
    {
    case ReadoutOrder::kRowMajor:
        return "ROW_MAJOR";
    case ReadoutOrder::kGlux1605Hdr4LaneInterleaved:
        return "GLUX1605_HDR_4LANE_INTERLEAVED_EFFECTIVE";
    default:
        return "UNKNOWN";
    }
}

bool ReadRaw12Pixel(const std::vector<uint8_t>& payload,
                    std::size_t source_pixel_index,
                    uint32_t width,
                    uint16_t* pixel12,
                    std::string* error)
{
    const std::size_t payload_offset = source_pixel_index * sizeof(uint16_t);
    const uint16_t pixel16 = network::ReadUint16LE(payload.data() + payload_offset);

    // 协议明确规定高 4 位是填充位且必须为 0。必须在掩码前检查；
    // 如果先执行 & 0x0FFF，位对齐、字节序或 FPGA 打包错误会被静默隐藏。
    if ((pixel16 & 0xF000u) != 0u)
    {
        const std::size_t x = source_pixel_index % static_cast<std::size_t>(width);
        const std::size_t y = source_pixel_index / static_cast<std::size_t>(width);
        std::ostringstream message;
        message << "RAW16 high bits are not zero at sourceIndex=" << source_pixel_index
                << ", x=" << x
                << ", y=" << y
                << ", rawValue=" << static_cast<unsigned int>(pixel16);
        protocol::SetImageProtocolError(error, "INVALID_HIGH_BITS", message.str());
        return false;
    }

    *pixel12 = static_cast<uint16_t>(pixel16 & 0x0FFFu);
    return true;
}

bool WritePixelToFrame(const std::vector<uint8_t>& payload,
                       std::size_t source_pixel_index,
                       std::size_t destination_pixel_index,
                       uint32_t width,
                       ConvertedImageFrame* frame,
                       std::string* error)
{
    uint16_t valid12 = 0u;
    if (!ReadRaw12Pixel(payload, source_pixel_index, width, &valid12, error))
    {
        return false;
    }

    frame->pixels16[destination_pixel_index] = valid12;
    frame->pixels8[destination_pixel_index] =
        static_cast<uint8_t>((static_cast<uint32_t>(valid12) * 255u) / 4095u);
    return true;
}

bool ConvertRowMajor(const std::vector<uint8_t>& payload,
                     uint32_t width,
                     std::size_t pixel_count,
                     ConvertedImageFrame* frame,
                     std::string* error)
{
    for (std::size_t pixel_index = 0u; pixel_index < pixel_count; ++pixel_index)
    {
        if (!WritePixelToFrame(payload, pixel_index, pixel_index, width, frame, error))
        {
            return false;
        }
    }
    return true;
}

bool ConvertGlux1605Hdr4LaneInterleaved(const std::vector<uint8_t>& payload,
                                        uint32_t width,
                                        uint32_t height,
                                        ConvertedImageFrame* frame,
                                        std::string* error)
{
    constexpr uint32_t kLaneCount = 4u;
    if ((width % kLaneCount) != 0u)
    {
        protocol::SetImageProtocolError(
            error,
            "UNSUPPORTED_READOUT_WIDTH",
            "GLUX1605 HDR 4-lane effective pixel reorder requires width divisible by 4");
        return false;
    }

    const std::size_t lane_width = static_cast<std::size_t>(width / kLaneCount);
    const std::size_t row_width = static_cast<std::size_t>(width);

    // Figure 42 的有效像素在 4 个 Sub-LVDS 通道上按同一采样时刻交织：
    // payload: lane0[k], lane1[k], lane2[k], lane3[k]
    // image:   x=k, x=laneWidth+k, x=2*laneWidth+k, x=3*laneWidth+k
    for (uint32_t y = 0u; y < height; ++y)
    {
        const std::size_t row_base = static_cast<std::size_t>(y) * row_width;
        for (std::size_t sample = 0u; sample < lane_width; ++sample)
        {
            for (uint32_t lane = 0u; lane < kLaneCount; ++lane)
            {
                const std::size_t source_index =
                    row_base + sample * kLaneCount + static_cast<std::size_t>(lane);
                const std::size_t destination_x =
                    static_cast<std::size_t>(lane) * lane_width + sample;
                const std::size_t destination_index = row_base + destination_x;
                if (!WritePixelToFrame(payload, source_index, destination_index, width, frame, error))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

}  // namespace

bool ConvertRaw16Low12ToGray(const std::vector<uint8_t>& payload,
                             uint32_t width,
                             uint32_t height,
                             ReadoutOrder readout_order,
                             ConvertedImageFrame* frame,
                             std::string* error)
{
    // 一帧像素总数固定由宽高决定，后续所有缓冲区大小都基于这个值。
    std::size_t expected_payload_size = 0u;
    if (!protocol::TryComputeRaw16Low12PayloadSize(width, height, &expected_payload_size))
    {
        protocol::SetImageProtocolError(
            error, "INVALID_DIMENSIONS", "RAW16 payload size calculation overflow");
        return false;
    }
    const std::size_t pixel_count = expected_payload_size / sizeof(uint16_t);

    // 当前协议中每个像素占 2 字节，因此 payload 长度必须严格等于 width * height * 2。
    if (payload.size() != expected_payload_size)
    {
        protocol::SetImageProtocolError(
            error, "PAYLOAD_LENGTH_MISMATCH", "RAW16(low12 valid) payload size mismatch");
        return false;
    }

    frame->width = width;
    frame->height = height;
    frame->fpga_payload = payload;
    frame->readout_order = ReadoutOrderName(readout_order);
    frame->pixels16.assign(pixel_count, 0u);
    frame->pixels8.assign(pixel_count, 0u);

    if (readout_order == ReadoutOrder::kGlux1605Hdr4LaneInterleaved)
    {
        return ConvertGlux1605Hdr4LaneInterleaved(payload, width, height, frame, error);
    }

    if (readout_order == ReadoutOrder::kRowMajor)
    {
        return ConvertRowMajor(payload, width, pixel_count, frame, error);
    }

    protocol::SetImageProtocolError(error, "UNSUPPORTED_READOUT_ORDER", "unsupported image readout order");
    return false;
}

bool ConvertHdrRaw16Low12ToPlanes(const std::vector<uint8_t>& payload,
                                  uint32_t width,
                                  uint32_t height,
                                  ReadoutOrder readout_order,
                                  ConvertedHdrImageFrame* frame,
                                  std::string* error)
{
    std::size_t single_plane_payload_size = 0u;
    if (!protocol::TryComputeRaw16Low12PayloadSize(width, height, &single_plane_payload_size))
    {
        protocol::SetImageProtocolError(
            error, "INVALID_DIMENSIONS", "HDR RAW16 payload size calculation overflow");
        return false;
    }

    if (single_plane_payload_size > std::numeric_limits<std::size_t>::max() / 2u ||
        payload.size() != single_plane_payload_size * 2u)
    {
        protocol::SetImageProtocolError(
            error,
            "HDR_PAYLOAD_LENGTH_MISMATCH",
            "HDR payload must contain exactly two RAW16 planes: HG followed by LG");
        return false;
    }

    const std::vector<uint8_t> hg_payload(
        payload.begin(),
        payload.begin() + static_cast<std::ptrdiff_t>(single_plane_payload_size));
    const std::vector<uint8_t> lg_payload(
        payload.begin() + static_cast<std::ptrdiff_t>(single_plane_payload_size),
        payload.end());

    ConvertedImageFrame hg_frame;
    if (!ConvertRaw16Low12ToGray(hg_payload, width, height, readout_order, &hg_frame, error))
    {
        return false;
    }

    ConvertedImageFrame lg_frame;
    if (!ConvertRaw16Low12ToGray(lg_payload, width, height, readout_order, &lg_frame, error))
    {
        return false;
    }

    frame->width = width;
    frame->height = height;
    frame->fpga_payload = payload;
    frame->readout_order = ReadoutOrderName(readout_order);
    frame->hg_pixels16 = std::move(hg_frame.pixels16);
    frame->lg_pixels16 = std::move(lg_frame.pixels16);
    return true;
}

}  // namespace image
}  // namespace spectra
