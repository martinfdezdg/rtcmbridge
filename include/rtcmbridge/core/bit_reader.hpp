#pragma once

#include <cstddef>
#include <cstdint>

namespace rtcmbridge {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t bytes)
        : data_(data), total_bits_(bytes * 8), bitpos_(0) {}

    bool getU(int n, uint64_t& out) {
        if (n < 0 || n > 64) return false;
        if (bitpos_ + static_cast<size_t>(n) > total_bits_) return false;

        uint64_t value = 0;
        for (int i = 0; i < n; ++i) {
            const size_t idx = bitpos_ + static_cast<size_t>(i);
            const uint8_t bit = (data_[idx / 8] >> (7U - (idx % 8U))) & 0x01U;
            value = (value << 1U) | static_cast<uint64_t>(bit);
        }

        bitpos_ += static_cast<size_t>(n);
        out = value;
        return true;
    }

    bool getS(int n, int64_t& out) {
        uint64_t u = 0;
        if (!getU(n, u)) return false;

        if (n == 0) {
            out = 0;
            return true;
        }

        if (((u >> (n - 1)) & 0x1U) != 0U) {
            const uint64_t mask = (~0ULL) << n;
            u |= mask;
        }
        out = static_cast<int64_t>(u);
        return true;
    }

private:
    const uint8_t* data_;
    size_t total_bits_;
    size_t bitpos_;
};

}  // namespace rtcmbridge
