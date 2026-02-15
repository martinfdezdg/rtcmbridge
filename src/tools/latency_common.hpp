#pragma once

#include "rtcmbridge/core/bit_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

namespace rtcmbridge::latency {

struct LatencySummary {
    bool has_samples = false;
    size_t count = 0;
    double last_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
};

class RollingLatencyStats {
public:
    explicit RollingLatencyStats(size_t max_samples = 4096) : max_samples_(max_samples) {}

    void add(double value_ms)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        last_ms_ = value_ms;
        ++total_samples_;
        samples_.push_back(value_ms);
        if (samples_.size() > max_samples_) samples_.pop_front();
    }

    size_t total_samples() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return total_samples_;
    }

    LatencySummary summary() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        LatencySummary out;
        out.count = samples_.size();
        if (samples_.empty()) return out;

        std::vector<double> sorted(samples_.begin(), samples_.end());
        std::sort(sorted.begin(), sorted.end());

        double sum = 0.0;
        for (double v : sorted) sum += v;

        out.has_samples = true;
        out.last_ms = last_ms_;
        out.min_ms = sorted.front();
        out.max_ms = sorted.back();
        out.mean_ms = sum / static_cast<double>(sorted.size());
        out.p50_ms = quantile(sorted, 0.50);
        out.p95_ms = quantile(sorted, 0.95);
        out.p99_ms = quantile(sorted, 0.99);
        return out;
    }

private:
    static double quantile(const std::vector<double>& sorted, double q)
    {
        if (sorted.empty()) return 0.0;
        const double idx = q * static_cast<double>(sorted.size() - 1);
        const size_t lo = static_cast<size_t>(std::floor(idx));
        const size_t hi = static_cast<size_t>(std::ceil(idx));
        if (lo == hi) return sorted[lo];
        const double t = idx - static_cast<double>(lo);
        return sorted[lo] + (sorted[hi] - sorted[lo]) * t;
    }

    size_t max_samples_;
    mutable std::mutex mtx_;
    std::deque<double> samples_;
    size_t total_samples_ = 0;
    double last_ms_ = 0.0;
};

inline bool extract_gps_msm_tow_ms(const uint8_t* payload, size_t payload_len, uint32_t& tow_ms, int& msg_type)
{
    BitReader br(payload, payload_len);
    uint64_t msg = 0;
    if (!br.getU(12, msg)) return false;
    msg_type = static_cast<int>(msg);

    // GPS MSM types 1071..1077 contain epoch time in milliseconds-of-week (30 bits).
    if (msg_type < 1071 || msg_type > 1077) return false;

    uint64_t station_id = 0;
    uint64_t tow = 0;
    if (!br.getU(12, station_id)) return false;
    if (!br.getU(30, tow)) return false;
    tow_ms = static_cast<uint32_t>(tow);
    return true;
}

inline int64_t gps_now_ms(int gps_utc_leap_sec)
{
    static constexpr int64_t kUnixToGpsEpochMs = 315964800000LL;  // 1980-01-06
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const int64_t unix_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return unix_ms + static_cast<int64_t>(gps_utc_leap_sec) * 1000LL - kUnixToGpsEpochMs;
}

inline std::optional<double> gps_msm_frame_latency_ms(const uint8_t* frame, size_t frame_len, int gps_utc_leap_sec)
{
    if (frame_len < 6) return std::nullopt;
    const uint8_t* payload = frame + 3;
    const size_t payload_len = frame_len - 6;

    uint32_t tow_ms = 0;
    int msg_type = -1;
    if (!extract_gps_msm_tow_ms(payload, payload_len, tow_ms, msg_type)) return std::nullopt;

    static constexpr int64_t kWeekMs = 604800000LL;
    int64_t now_gps_ms = gps_now_ms(gps_utc_leap_sec);
    int64_t week_start = (now_gps_ms / kWeekMs) * kWeekMs;
    int64_t frame_gps_ms = week_start + static_cast<int64_t>(tow_ms);

    // Align with nearest GPS week to avoid rollover ambiguities.
    int64_t diff = now_gps_ms - frame_gps_ms;
    if (diff > (kWeekMs / 2)) frame_gps_ms += kWeekMs;
    else if (diff < -(kWeekMs / 2)) frame_gps_ms -= kWeekMs;

    const double latency_ms = static_cast<double>(now_gps_ms - frame_gps_ms);
    // Drop obviously invalid values from rollover/unsupported payload.
    if (std::fabs(latency_ms) > 60000.0) return std::nullopt;
    return latency_ms;
}

}  // namespace rtcmbridge::latency

