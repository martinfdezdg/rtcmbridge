#include "rtcmbridge/core/rtcm_1005.hpp"

#include "rtcmbridge/core/bit_reader.hpp"

namespace rtcmbridge {

namespace {

constexpr double kCoordScale = 0.0001;
constexpr double kHeightScale = 0.0001;

}  // namespace

int rtcm_message_type(const uint8_t* payload, size_t payload_len)
{
    if (payload_len < 2) return -1;
    BitReader br(payload, payload_len);
    uint64_t type = 0;
    if (!br.getU(12, type)) return -1;
    return static_cast<int>(type);
}

std::optional<StationAntennaPosition> decode_station_position_1005_1006(
    const uint8_t* payload,
    size_t payload_len)
{
    BitReader br(payload, payload_len);

    uint64_t msg_type_u = 0;
    if (!br.getU(12, msg_type_u)) return std::nullopt;
    const int msg_type = static_cast<int>(msg_type_u);
    if (msg_type != 1005 && msg_type != 1006) return std::nullopt;

    uint64_t station_id = 0;
    uint64_t ignored_u = 0;
    int64_t x_raw = 0;
    int64_t y_raw = 0;
    int64_t z_raw = 0;

    if (!br.getU(12, station_id)) return std::nullopt;
    if (!br.getU(6, ignored_u)) return std::nullopt;
    if (!br.getU(1, ignored_u)) return std::nullopt;
    if (!br.getU(1, ignored_u)) return std::nullopt;
    if (!br.getU(1, ignored_u)) return std::nullopt;
    if (!br.getU(1, ignored_u)) return std::nullopt;
    if (!br.getS(38, x_raw)) return std::nullopt;
    if (!br.getU(1, ignored_u)) return std::nullopt;
    if (!br.getU(1, ignored_u)) return std::nullopt;
    if (!br.getS(38, y_raw)) return std::nullopt;
    if (!br.getU(2, ignored_u)) return std::nullopt;
    if (!br.getS(38, z_raw)) return std::nullopt;

    StationAntennaPosition out;
    out.message_type = msg_type;
    out.station_id = static_cast<int>(station_id);
    out.x_m = static_cast<double>(x_raw) * kCoordScale;
    out.y_m = static_cast<double>(y_raw) * kCoordScale;
    out.z_m = static_cast<double>(z_raw) * kCoordScale;

    if (msg_type == 1006) {
        uint64_t h_raw = 0;
        if (!br.getU(16, h_raw)) return std::nullopt;
        out.has_antenna_height = true;
        out.antenna_height_m = static_cast<double>(h_raw) * kHeightScale;
    }

    return out;
}

}  // namespace rtcmbridge
