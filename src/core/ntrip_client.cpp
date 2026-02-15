#include "rtcmbridge/core/ntrip_client.hpp"

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

using boost::asio::ip::tcp;

namespace rtcmbridge {

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string make_request(const NtripConfig& cfg)
{
    std::ostringstream oss;
    oss << "GET /" << cfg.mountpoint << " HTTP/1.1\r\n"
        << "User-Agent: rtcmbridge/1.0\r\n"
        << "Authorization: Basic "
        << NtripStreamClient::base64_encode(cfg.user + ":" + cfg.pass) << "\r\n"
        << "Connection: keep-alive\r\n\r\n";
    return oss.str();
}

bool is_ok_status(const std::string& header)
{
    return header.rfind("ICY 200", 0) == 0 ||
           header.rfind("HTTP/1.0 200", 0) == 0 ||
           header.rfind("HTTP/1.1 200", 0) == 0;
}

}  // namespace

std::string NtripStreamClient::base64_encode(const std::string& in)
{
    std::string out;
    out.reserve((in.size() * 4U) / 3U + 4U);
    int val = 0;
    int valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kB64[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(kB64[((val << 8) >> (valb + 8)) & 0x3F]);
    while ((out.size() % 4U) != 0U) out.push_back('=');
    return out;
}

void NtripStreamClient::log(const Logger& logger, const std::string& msg)
{
    if (logger) logger(msg);
}

int NtripStreamClient::run(const NtripConfig& cfg,
                           const OnBytes& on_bytes,
                           std::atomic<bool>& stop_flag,
                           const Logger& logger) const
{
    if (cfg.host.empty() || cfg.mountpoint.empty()) {
        log(logger, "NTRIP config error: host/mountpoint required");
        return 2;
    }

    while (!stop_flag.load()) {
        try {
            boost::asio::io_context io;
            tcp::resolver resolver(io);
            tcp::socket socket(io);

            auto endpoints = resolver.resolve(cfg.host, std::to_string(cfg.port));
            boost::asio::connect(socket, endpoints);

            const std::string request = make_request(cfg);
            boost::asio::write(socket, boost::asio::buffer(request));

            std::string header;
            std::array<char, 4096> tmp{};
            bool header_done = false;

            while (!stop_flag.load()) {
                const size_t n = socket.read_some(boost::asio::buffer(tmp));
                if (n == 0) break;

                if (!header_done) {
                    header.append(tmp.data(), n);
                    const size_t p = header.find("\r\n\r\n");
                    if (p == std::string::npos) continue;
                    header_done = true;

                    if (!is_ok_status(header)) {
                        log(logger, "Caster response rejected: " + header.substr(0, header.find("\r\n")));
                        break;
                    }

                    const size_t body_start = p + 4U;
                    if (body_start < header.size()) {
                        const uint8_t* body = reinterpret_cast<const uint8_t*>(header.data() + body_start);
                        const size_t body_len = header.size() - body_start;
                        if (!on_bytes(body, body_len)) {
                            stop_flag = true;
                            break;
                        }
                    }
                    header.clear();
                    continue;
                }

                if (!on_bytes(reinterpret_cast<const uint8_t*>(tmp.data()), n)) {
                    stop_flag = true;
                    break;
                }
            }
        } catch (const std::exception& ex) {
            log(logger, std::string("NTRIP connection error: ") + ex.what());
        }

        if (!stop_flag.load()) {
            const int d = (cfg.reconnect_delay_seconds > 0) ? cfg.reconnect_delay_seconds : 1;
            log(logger, "Reconnecting in " + std::to_string(d) + "s");
            std::this_thread::sleep_for(std::chrono::seconds(d));
        }
    }

    return 0;
}

}  // namespace rtcmbridge
