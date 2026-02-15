#include "rtcmbridge/core/rtcm_1005.hpp"
#include "rtcmbridge/core/rtcm_frame.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace rtcmbridge;

namespace {

class BitWriter {
public:
    void put_u(int n, uint64_t value) {
        for (int i = n - 1; i >= 0; --i) {
            const uint8_t bit = static_cast<uint8_t>((value >> i) & 1ULL);
            push_bit(bit);
        }
    }

    void put_s(int n, int64_t value) {
        uint64_t enc = 0;
        if (value < 0) {
            const uint64_t mod = (n == 64) ? 0ULL : (1ULL << n);
            enc = mod + static_cast<uint64_t>(value);
        } else {
            enc = static_cast<uint64_t>(value);
        }
        put_u(n, enc);
    }

    const std::vector<uint8_t>& bytes() {
        if (bit_pos_ != 0) {
            data_.push_back(current_byte_);
            current_byte_ = 0;
            bit_pos_ = 0;
        }
        return data_;
    }

private:
    void push_bit(uint8_t bit) {
        current_byte_ = static_cast<uint8_t>((current_byte_ << 1U) | (bit & 0x1U));
        ++bit_pos_;
        if (bit_pos_ == 8) {
            data_.push_back(current_byte_);
            current_byte_ = 0;
            bit_pos_ = 0;
        }
    }

    std::vector<uint8_t> data_;
    uint8_t current_byte_ = 0;
    int bit_pos_ = 0;
};

[[nodiscard]] int fail(const std::string& msg)
{
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
}

std::vector<uint8_t> make_valid_rtcm_frame(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + 6U);
    frame.push_back(0xD3);
    frame.push_back(static_cast<uint8_t>((payload.size() >> 8U) & 0x03U));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xFFU));
    frame.insert(frame.end(), payload.begin(), payload.end());

    const uint32_t crc = crc24q(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>((crc >> 16U) & 0xFFU));
    frame.push_back(static_cast<uint8_t>((crc >> 8U) & 0xFFU));
    frame.push_back(static_cast<uint8_t>(crc & 0xFFU));
    return frame;
}

int test_frame_parser_valid_and_bad_crc()
{
    RtcmFrameParser parser;
    int cb_count = 0;

    std::vector<uint8_t> payload{0x3F, 0xF0, 0xAA, 0x55};
    std::vector<uint8_t> frame = make_valid_rtcm_frame(payload);

    parser.push(frame.data(), 2, [&](const uint8_t*, size_t) { ++cb_count; });
    parser.push(frame.data() + 2, frame.size() - 2, [&](const uint8_t*, size_t) { ++cb_count; });

    if (cb_count != 1) return fail("expected 1 parsed frame");
    if (parser.frames_ok() != 1) return fail("frames_ok should be 1");
    if (parser.frames_bad_crc() != 0) return fail("frames_bad_crc should be 0");

    frame.back() ^= 0x01U;
    parser.push(frame.data(), frame.size(), [&](const uint8_t*, size_t) { ++cb_count; });

    if (parser.frames_bad_crc() != 1) return fail("frames_bad_crc should be 1 after bad frame");
    return 0;
}

int test_decode_1006()
{
    BitWriter bw;
    bw.put_u(12, 1006);   // message type
    bw.put_u(12, 42);     // station id
    bw.put_u(6, 0);       // ITRF
    bw.put_u(1, 1);       // GPS indicator
    bw.put_u(1, 1);       // GLONASS indicator
    bw.put_u(1, 1);       // Galileo indicator
    bw.put_u(1, 0);       // reference station indicator
    bw.put_s(38, 12345);  // x 1.2345 m
    bw.put_u(1, 0);       // single receiver oscillator
    bw.put_u(1, 0);       // reserved
    bw.put_s(38, -23456); // y -2.3456 m
    bw.put_u(2, 0);       // quarter cycle indicators
    bw.put_s(38, 34567);  // z 3.4567 m
    bw.put_u(16, 789);    // antenna height 0.0789 m

    const auto payload = bw.bytes();
    const auto p = decode_station_position_1005_1006(payload.data(), payload.size());
    if (!p.has_value()) return fail("decode_station_position_1005_1006 returned nullopt");

    if (p->message_type != 1006) return fail("message_type != 1006");
    if (p->station_id != 42) return fail("station_id != 42");

    if (std::fabs(p->x_m - 1.2345) > 1e-6) return fail("x mismatch");
    if (std::fabs(p->y_m + 2.3456) > 1e-6) return fail("y mismatch");
    if (std::fabs(p->z_m - 3.4567) > 1e-6) return fail("z mismatch");

    if (!p->has_antenna_height) return fail("expected antenna height in 1006");
    if (std::fabs(p->antenna_height_m - 0.0789) > 1e-6) return fail("antenna height mismatch");

    const int type = rtcm_message_type(payload.data(), payload.size());
    if (type != 1006) return fail("rtcm_message_type != 1006");

    return 0;
}

}  // namespace

int main()
{
    if (const int rc = test_frame_parser_valid_and_bad_crc(); rc != 0) return rc;
    if (const int rc = test_decode_1006(); rc != 0) return rc;

    std::cout << "PASS: rtcm core tests\n";
    return 0;
}
