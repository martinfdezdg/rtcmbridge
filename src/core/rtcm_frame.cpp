#include "rtcmbridge/core/rtcm_frame.hpp"

#include <algorithm>

namespace rtcmbridge {

uint32_t crc24q(const uint8_t* data, size_t len)
{
    static constexpr uint32_t poly = 0x1864CFB;
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 16;
        for (int b = 0; b < 8; ++b) {
            crc <<= 1;
            if ((crc & 0x1000000U) != 0U) crc ^= poly;
        }
    }
    return crc & 0xFFFFFFU;
}

void RtcmFrameParser::push(const uint8_t* data, size_t len, const OnFrame& on_frame)
{
    buffer_.insert(buffer_.end(), data, data + len);

    while (buffer_.size() >= 6) {
        auto it = std::find(buffer_.begin(), buffer_.end(), static_cast<uint8_t>(0xD3));
        if (it == buffer_.end()) {
            buffer_.clear();
            return;
        }

        if (it != buffer_.begin()) buffer_.erase(buffer_.begin(), it);
        if (buffer_.size() < 6) return;

        const uint16_t payload_len =
            static_cast<uint16_t>(((buffer_[1] & 0x03U) << 8U) | buffer_[2]);
        const size_t frame_len = 3U + payload_len + 3U;
        if (buffer_.size() < frame_len) return;

        const uint32_t got_crc = (static_cast<uint32_t>(buffer_[frame_len - 3]) << 16U) |
                                 (static_cast<uint32_t>(buffer_[frame_len - 2]) << 8U) |
                                 static_cast<uint32_t>(buffer_[frame_len - 1]);
        const uint32_t calc_crc = crc24q(buffer_.data(), frame_len - 3U);

        if (got_crc == calc_crc) {
            ++frames_ok_;
            on_frame(buffer_.data(), frame_len);
        } else {
            ++frames_bad_crc_;
        }

        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame_len));
    }
}

}  // namespace rtcmbridge
