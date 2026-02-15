#include "latency_common.hpp"

#include "rtcmbridge/core/rtcm_frame.hpp"

#include <nats/nats.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rtcmbridge;

namespace {

std::atomic<bool> g_stop{false};

struct AppConfig {
    std::string mountpoint;
    std::string nats_servers_csv = "nats://127.0.0.1:4222";
    int summary_sec = 5;
    int gps_utc_leap_sec = 18;
};

struct RuntimeCtx {
    RtcmFrameParser parser;
    latency::RollingLatencyStats stats{4096};
    std::mutex mtx;
    std::atomic<uint64_t> chunks_total{0};
    std::atomic<uint64_t> frames_total{0};
    std::atomic<uint64_t> frames_with_latency{0};
    int gps_utc_leap_sec = 18;
};

void on_signal(int)
{
    g_stop = true;
}

void print_usage()
{
    std::cerr << "Usage: rtcm_latency_nats --mountpoint=<name> "
              << "[--nats=nats://127.0.0.1:4222] "
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
        else if (k == "nats") cfg.nats_servers_csv = v;
        else if (k == "summary-sec") cfg.summary_sec = std::max(1, std::stoi(v));
        else if (k == "gps-utc-leap-sec") cfg.gps_utc_leap_sec = std::stoi(v);
    }
    return !cfg.mountpoint.empty();
}

std::vector<std::string> split_csv(const std::string& csv)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= csv.size()) {
        size_t end = csv.find(',', start);
        if (end == std::string::npos) end = csv.size();
        std::string item = csv.substr(start, end - start);
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char c) {
            return std::isspace(c) == 0;
        }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char c) {
            return std::isspace(c) == 0;
        }).base(), item.end());
        if (!item.empty()) out.push_back(item);
        start = end + 1;
    }
    return out;
}

void print_summary(const std::string& mountpoint, const RuntimeCtx& ctx)
{
    const auto s = ctx.stats.summary();
    if (!s.has_samples) {
        std::cout << "[latency][nats][" << mountpoint << "] "
                  << "chunks=" << ctx.chunks_total.load()
                  << " frames=" << ctx.frames_total.load()
                  << " gps_msm_frames=0 (waiting for RTCM 1071..1077)\n";
        return;
    }

    std::cout << "[latency][nats][" << mountpoint << "] "
              << "chunks=" << ctx.chunks_total.load()
              << " frames=" << ctx.frames_total.load()
              << " gps_msm_frames=" << ctx.frames_with_latency.load()
              << " samples=" << s.count
              << " last=" << s.last_ms << "ms"
              << " mean=" << s.mean_ms << "ms"
              << " p50=" << s.p50_ms << "ms"
              << " p95=" << s.p95_ms << "ms"
              << " p99=" << s.p99_ms << "ms"
              << " min=" << s.min_ms << "ms"
              << " max=" << s.max_ms << "ms\n";
}

void on_msg(natsConnection*, natsSubscription*, natsMsg* msg, void* closure)
{
    auto* ctx = static_cast<RuntimeCtx*>(closure);
    if (msg == nullptr || ctx == nullptr) return;

    const char* data = natsMsg_GetData(msg);
    const int data_len = natsMsg_GetDataLength(msg);
    if (data != nullptr && data_len > 0) {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        ctx->chunks_total.fetch_add(1);
        ctx->parser.push(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(data_len),
                         [&](const uint8_t* frame, size_t frame_len) {
                             ctx->frames_total.fetch_add(1);
                             const auto lat_ms =
                                 latency::gps_msm_frame_latency_ms(frame, frame_len, ctx->gps_utc_leap_sec);
                             if (lat_ms.has_value()) {
                                 ctx->frames_with_latency.fetch_add(1);
                                 ctx->stats.add(*lat_ms);
                             }
                         });
    }

    natsMsg_Destroy(msg);
}

}  // namespace

int main(int argc, char* argv[])
{
    AppConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage();
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    natsOptions* opts = nullptr;
    natsConnection* nc = nullptr;
    natsSubscription* sub = nullptr;
    natsOptions_Create(&opts);

    const auto servers = split_csv(cfg.nats_servers_csv);
    std::vector<const char*> c_servers;
    c_servers.reserve(servers.size());
    for (const auto& s : servers) c_servers.push_back(s.c_str());
    if (!c_servers.empty()) {
        natsOptions_SetServers(opts, c_servers.data(), static_cast<int>(c_servers.size()));
    }

    natsStatus st = natsConnection_Connect(&nc, opts);
    if (st != NATS_OK) {
        std::cerr << "NATS connection failed: " << natsStatus_GetText(st) << "\n";
        natsOptions_Destroy(opts);
        return 1;
    }

    RuntimeCtx ctx;
    ctx.gps_utc_leap_sec = cfg.gps_utc_leap_sec;
    const std::string subject = "NTRIP." + cfg.mountpoint;

    st = natsConnection_Subscribe(&sub, nc, subject.c_str(), on_msg, &ctx);
    if (st != NATS_OK) {
        std::cerr << "NATS subscribe failed: " << natsStatus_GetText(st) << "\n";
        natsConnection_Destroy(nc);
        natsOptions_Destroy(opts);
        return 1;
    }

    std::cout << "[latency][nats][" << cfg.mountpoint << "] subscribed to " << subject << "\n";

    auto next_summary = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.summary_sec);
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_summary) {
            print_summary(cfg.mountpoint, ctx);
            next_summary = now + std::chrono::seconds(cfg.summary_sec);
        }
    }

    print_summary(cfg.mountpoint, ctx);
    if (sub != nullptr) natsSubscription_Destroy(sub);
    natsConnection_Destroy(nc);
    natsOptions_Destroy(opts);
    return 0;
}
