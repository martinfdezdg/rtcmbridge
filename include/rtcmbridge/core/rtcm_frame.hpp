#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace rtcmbridge {

uint32_t crc24q(const uint8_t* data, size_t len);

class RtcmFrameParser {
public:
    using OnFrame = std::function<void(const uint8_t*, size_t)>;

    void push(const uint8_t* data, size_t len, const OnFrame& on_frame);

    size_t frames_ok() const { return frames_ok_; }
    size_t frames_bad_crc() const { return frames_bad_crc_; }

private:
    std::vector<uint8_t> buffer_;
    size_t frames_ok_ = 0;
    size_t frames_bad_crc_ = 0;
};

}  // namespace rtcmbridge
