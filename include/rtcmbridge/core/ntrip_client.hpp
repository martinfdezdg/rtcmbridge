#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace rtcmbridge {

struct NtripConfig {
    std::string host;
    int port = 2101;
    std::string mountpoint;
    std::string user;
    std::string pass;
    int reconnect_delay_seconds = 3;
};

class NtripStreamClient {
public:
    using OnBytes = std::function<bool(const uint8_t*, size_t)>;
    using Logger = std::function<void(const std::string&)>;

    int run(const NtripConfig& cfg,
            const OnBytes& on_bytes,
            std::atomic<bool>& stop_flag,
            const Logger& logger = nullptr) const;

    static std::string base64_encode(const std::string& in);

private:
    static void log(const Logger& logger, const std::string& msg);
};

}  // namespace rtcmbridge
