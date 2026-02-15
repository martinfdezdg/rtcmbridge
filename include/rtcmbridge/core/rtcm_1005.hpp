#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace rtcmbridge {

struct StationAntennaPosition {
    int message_type = 0;
    int station_id = 0;
    double x_m = 0.0;
    double y_m = 0.0;
    double z_m = 0.0;
    bool has_antenna_height = false;
    double antenna_height_m = 0.0;
};

int rtcm_message_type(const uint8_t* payload, size_t payload_len);
std::optional<StationAntennaPosition> decode_station_position_1005_1006(
    const uint8_t* payload,
    size_t payload_len);

}  // namespace rtcmbridge
