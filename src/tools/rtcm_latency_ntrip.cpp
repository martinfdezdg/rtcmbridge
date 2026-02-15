#include "latency_common.hpp"

#include "rtcmbridge/core/mountpoint_config.hpp"
#include "rtcmbridge/core/ntrip_client.hpp"
#include "rtcmbridge/core/rtcm_frame.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace rtcmbridge;

namespace {

std::atomic<bool> g_stop{false};

struct AppConfig {
    std::string mountpoints_file = "mountpoints.conf";
    std::string mountpoint;
    int summary_sec = 5;
    int gps_utc_leap_sec = 18;
};

void on_signal(int)
{
    g_stop = true;
}

void print_usage()
{
    std::cerr << "Usage: rtcm_latency_ntrip --mountpoint=<name> "
              << "[--mountpoints-file=mountpoints.conf] "
              << "[--summary-sec=5] [--gps-utc-leap-sec=18]\n";
}

bool parse_args(int argc, char* argv[], AppConfig& cfg)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") return false;
        if (arg.rfind("--", 0) != 0) continue;
        const size_t eq = arg.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = arg.substr(2, eq - 2);
        const std::string v = arg.substr(eq + 1);
        if (k == "mountpoint") cfg.mountpoint = v;
        else if (k == "mountpoints-file") cfg.mountpoints_file = v;
        else if (k == "summary-sec") cfg.summary_sec = std::max(1, std::stoi(v));
        else if (k == "gps-utc-leap-sec") cfg.gps_utc_leap_sec = std::stoi(v);
    }
    return !cfg.mountpoint.empty();
}

void print_summary(const std::string& mountpoint,
                   const latency::RollingLatencyStats& stats,
                   uint64_t frames_total,
                   uint64_t frames_with_latency)
{
    const auto s = stats.summary();
    if (!s.has_samples) {
        std::cout << "[latency][ntrip][" << mountpoint << "] "
                  << "frames=" << frames_total
                  << " gps_msm_frames=0 (waiting for RTCM 1071..1077)\n";
        return;
    }

    std::cout << "[latency][ntrip][" << mountpoint << "] "
              << "frames=" << frames_total
              << " gps_msm_frames=" << frames_with_latency
              << " samples=" << s.count
              << " last=" << s.last_ms << "ms"
              << " mean=" << s.mean_ms << "ms"
              << " p50=" << s.p50_ms << "ms"
              << " p95=" << s.p95_ms << "ms"
              << " p99=" << s.p99_ms << "ms"
              << " min=" << s.min_ms << "ms"
              << " max=" << s.max_ms << "ms\n";
}

}  // namespace

int main(int argc, char* argv[])
{
    AppConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage();
        return 1;
    }

    const auto all = load_all_ntrip_configs_from_mountpoints(cfg.mountpoints_file);
    if (all.empty()) {
        std::cerr << "No valid mountpoints in " << cfg.mountpoints_file << "\n";
        return 1;
    }

    std::optional<NtripConfig> selected;
    for (const auto& mp : all) {
        if (mp.mountpoint == cfg.mountpoint) {
            selected = mp;
            break;
        }
    }
    if (!selected.has_value()) {
        std::cerr << "Mountpoint '" << cfg.mountpoint << "' not found in "
                  << cfg.mountpoints_file << "\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    latency::RollingLatencyStats stats(4096);
    RtcmFrameParser parser;
    NtripStreamClient client;

    uint64_t frames_total = 0;
    uint64_t frames_with_latency = 0;
    auto next_summary = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.summary_sec);

    std::cout << "[latency][ntrip][" << cfg.mountpoint << "] "
              << "connecting to " << selected->host << ":" << selected->port << "\n";

    const int rc = client.run(
        *selected,
        [&](const uint8_t* data, size_t len) {
            parser.push(data, len, [&](const uint8_t* frame, size_t frame_len) {
                ++frames_total;
                const auto lat_ms = latency::gps_msm_frame_latency_ms(frame, frame_len, cfg.gps_utc_leap_sec);
                if (lat_ms.has_value()) {
                    ++frames_with_latency;
                    stats.add(*lat_ms);
                }
            });

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_summary) {
                print_summary(cfg.mountpoint, stats, frames_total, frames_with_latency);
                next_summary = now + std::chrono::seconds(cfg.summary_sec);
            }
            return !g_stop.load();
        },
        g_stop,
        [&](const std::string& msg) {
            std::cout << "[latency][ntrip][" << cfg.mountpoint << "] " << msg << "\n";
        });

    print_summary(cfg.mountpoint, stats, frames_total, frames_with_latency);
    return rc;
}
