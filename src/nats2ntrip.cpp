/*
 * nats2ntrip.cpp
 * NATS -> NTRIP bridge
 * Copyright (c) 2026 Martin
 * Licensed for internal / evaluation use.
 */

#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <nats/nats.h>
#include <ntrip/ntrip.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <logger/logger.h>

static constexpr const char* kSubjectPrefix = "NTRIP.";
static constexpr const char* kUserAgent = "NTRIP-CppBridge";

namespace {
namespace parser {

enum class Section {
    Options,
    NatsSources,
    NatsMountpoints,
    NtripDestinations
};

struct Configuration {
    configuration::Options options;
    std::vector<configuration::NatsDestination> nats_sources;
    std::vector<std::string> nats_mountpoints;
    configuration::NtripSource ntrip_destination;
    bool has_ntrip_destination = false;
};

bool parse_positive_int(const std::string& value, int& out)
{
    try {
        size_t pos = 0;
        const int parsed = std::stoi(value, &pos);
        if (pos != value.size() || parsed <= 0) return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_section_header(const std::string& line, Section& section, std::string& error)
{
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        error = "Invalid section header";
        return false;
    }

    const std::string section_line = boost::algorithm::to_lower_copy(line.substr(1, line.size() - 2));
    if (section_line == "options") {
        section = Section::Options;
        return true;
    }
    if (section_line == "nats_sources") {
        section = Section::NatsSources;
        return true;
    }
    if (section_line == "nats_mountpoints" || section_line == "ntrip_sources") {
        section = Section::NatsMountpoints;
        return true;
    }
    if (section_line == "ntrip_destinations") {
        section = Section::NtripDestinations;
        return true;
    }

    error = "Unknown section '" + section_line + "'";
    return false;
}

bool parse_options_body_line(const std::string& line,
                             Configuration& configuration,
                             std::string& error)
{
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
        error = "Invalid option format";
        return false;
    }

    std::string key = boost::algorithm::to_lower_copy(boost::algorithm::trim_copy(line.substr(0, eq)));
    std::string value = boost::algorithm::trim_copy(line.substr(eq + 1));
    if (key.empty() || value.empty()) {
        error = "Invalid option format";
        return false;
    }

    if (key == "read_buffer_bytes") {
        if (!parse_positive_int(value, configuration.options.read_buffer_bytes)) {
            error = "Invalid option value 'read_buffer_bytes'";
            return false;
        }
        return true;
    }

    if (key == "reconnect_min_sec") {
        if (!parse_positive_int(value, configuration.options.reconnect_min_sec)) {
            error = "Invalid option value 'reconnect_min_sec'";
            return false;
        }
        return true;
    }

    if (key == "reconnect_max_sec") {
        if (!parse_positive_int(value, configuration.options.reconnect_max_sec)) {
            error = "Invalid option value 'reconnect_max_sec'";
            return false;
        }
        return true;
    }

    if (key == "throughput_log_kb") {
        if (!parse_positive_int(value, configuration.options.throughput_log_every_bytes)) {
            error = "Invalid option value 'throughput_log_kb'";
            return false;
        }
        configuration.options.throughput_log_every_bytes *= 1024U;
        return true;
    }

    error = "Invalid option key '" + key + "'";
    return false;
}

bool parse_nats_sources_body_line(const std::string& line,
                                  Configuration& configuration,
                                  std::string& error)
{
    const std::string prefix = "nats://";
    if (line.substr(0, prefix.size()) != prefix) {
        error = "Invalid NATS source format";
        return false;
    }
    const auto host_limit = line.find(':', prefix.size());
    if (host_limit == std::string::npos || host_limit == 0) {
        error = "Invalid NATS source format: missing ':'";
        return false;
    }
    int port = 0;
    if (!parse_positive_int(line.substr(host_limit + 1), port)) {
        error = "Invalid NATS source format: invalid port";
        return false;
    }

    configuration::NatsDestination nats_source;
    nats_source.server = prefix + line.substr(prefix.size(), host_limit - prefix.size()) + ":" + std::to_string(port);
    configuration.nats_sources.push_back(std::move(nats_source));
    return true;
}

bool parse_nats_mountpoint_body_line(const std::string& line,
                                             Configuration& configuration,
                                             std::string& error)
{
    const std::string mountpoint = boost::algorithm::trim_copy(line);
    if (mountpoint.empty()) {
        error = "Invalid NTRIP source format: empty mountpoint";
        return false;
    }
    if (mountpoint.find_first_of(" \t/:") != std::string::npos) {
        error = "Invalid NTRIP source format: mountpoint must be a plain name";
        return false;
    }

    configuration.nats_mountpoints.push_back(mountpoint);
    return true;
}

bool parse_ntrip_destination_body_line(const std::string& line,
                                       Configuration& configuration,
                                       std::string& error)
{
    const auto user_limit = line.find(':');
    if (user_limit == std::string::npos || user_limit == 0) {
        error = "Invalid NTRIP destination format: missing ':'";
        return false;
    }
    const auto pass_limit = line.find(' ', user_limit + 1);
    if (pass_limit == std::string::npos || pass_limit == user_limit + 1) {
        error = "Invalid NTRIP destination format: missing space";
        return false;
    }
    const auto host_limit = line.find(':', pass_limit + 1);
    if (host_limit == std::string::npos || host_limit == pass_limit + 1) {
        error = "Invalid NTRIP destination format: missing ':'";
        return false;
    }
    if (line.find('/', host_limit + 1) != std::string::npos) {
        error = "Invalid NTRIP destination format: destination must not include a mountpoint";
        return false;
    }

    configuration.ntrip_destination.user = line.substr(0, user_limit);
    configuration.ntrip_destination.pass = line.substr(user_limit + 1, pass_limit - user_limit - 1);
    configuration.ntrip_destination.host = line.substr(pass_limit + 1, host_limit - pass_limit - 1);
    if (!parse_positive_int(line.substr(host_limit + 1), configuration.ntrip_destination.port)) {
        error = "Invalid NTRIP destination format: invalid port";
        return false;
    }
    configuration.ntrip_destination.name.clear();
    configuration.has_ntrip_destination = true;
    return true;
}

bool parse_configuration(const std::string& path, Configuration& configuration, std::string& error)
{
    std::ifstream in(path);
    if (!in) {
        error = "Cannot open configuration file " + path;
        return false;
    }

    Section section = Section::Options;

    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed_line = boost::algorithm::trim_copy(line);

        if (!trimmed_line.empty() && trimmed_line.front() != '#') {
            if (trimmed_line.front() == '[') {
                if (!parse_section_header(trimmed_line, section, error)) {
                    return false;
                }
            } else {
                switch (section) {
                    case Section::Options:
                        if (!parse_options_body_line(trimmed_line, configuration, error)) {
                            return false;
                        }
                        break;
                    case Section::NatsSources:
                        if (!parse_nats_sources_body_line(trimmed_line, configuration, error)) {
                            return false;
                        }
                        break;
                    case Section::NatsMountpoints:
                        if (!parse_nats_mountpoint_body_line(trimmed_line, configuration, error)) {
                            return false;
                        }
                        break;
                    case Section::NtripDestinations:
                        if (!parse_ntrip_destination_body_line(trimmed_line, configuration, error)) {
                            return false;
                        }
                        break;
                }
            }
        }
    }

    if (configuration.options.reconnect_min_sec > configuration.options.reconnect_max_sec) {
        error = "Value of reconnect_min_sec cannot be greater than reconnect_max_sec";
        return false;
    }
    if (configuration.nats_sources.empty()) {
        error = "No NATS sources configured";
        return false;
    }
    if (configuration.nats_mountpoints.empty()) {
        error = "No NATS mountpoints configured";
        return false;
    }
    if (!configuration.has_ntrip_destination) {
        error = "No NTRIP destination configured";
        return false;
    }

    return true;
}

std::string nats_connected_url(natsConnection* nc)
{
    if (nc == nullptr) return {};
    char buf[256] = {0};
    const natsStatus s = natsConnection_GetConnectedUrl(nc, buf, sizeof(buf));
    if (s != NATS_OK) return {};
    return std::string(buf);
}

void nats_on_disconnected(natsConnection* nc, void* closure)
{
    auto* logger = static_cast<Logger*>(closure);
    if (logger == nullptr) return;
    const std::string url = nats_connected_url(nc);
    logger->warn("NATS", url.empty() ? "Disconnected" : ("Disconnected from " + url));
}

void nats_on_reconnected(natsConnection* nc, void* closure)
{
    auto* logger = static_cast<Logger*>(closure);
    if (logger == nullptr) return;
    const std::string url = nats_connected_url(nc);
    logger->info("NATS", url.empty() ? "Reconnected" : ("Reconnected to " + url));
}

void nats_on_closed(natsConnection* /*nc*/, void* closure)
{
    auto* logger = static_cast<Logger*>(closure);
    if (logger == nullptr) return;
    logger->error("NATS", "Connection closed (no more reconnect attempts)");
}

void nats_on_async_error(natsConnection* /*nc*/,
                         natsSubscription* /*sub*/,
                         natsStatus err,
                         void* closure)
{
    auto* logger = static_cast<Logger*>(closure);
    if (logger == nullptr) return;
    logger->warn("NATS", "async error: " + std::string(natsStatus_GetText(err)));
}

std::atomic<bool> g_stop(false);
std::condition_variable g_stop_cv;
std::mutex g_stop_mtx;

void handle_signal(int)
{
    g_stop = true;
    g_stop_cv.notify_all();
}

}  // namespace parser
}  // namespace

class StreamSession {
public:
    StreamSession(ntripConnection* ntrip_connection,
                  natsConnection* nats_connection,
                  Logger& logger)
        : ntrip_connection_(ntrip_connection),
          nats_connection_(nats_connection),
          logger_(logger),
          subject_(std::string(kSubjectPrefix) + std::string(ntripSource_GetText(ntrip_connection))),
          reconnect_delay_(ntrip_connection->options().reconnect_min_sec())
    {
    }

    ~StreamSession()
    {
        stop();
    }

    void start()
    {
        logger_.info(subject_, "Starting session");

        const natsStatus subscribe_status =
            natsConnection_Subscribe(&subscription_, nats_connection_, subject_.c_str(), &StreamSession::on_nats_msg, this);
        if (subscribe_status != NATS_OK) {
            throw std::runtime_error("Subscribe failed on " + subject_ + ": " + std::string(natsStatus_GetText(subscribe_status)));
        }

        worker_ = std::thread([this] { run(); });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();

        if (subscription_ != nullptr) {
            natsSubscription_Unsubscribe(subscription_);
            natsSubscription_Destroy(subscription_);
            subscription_ = nullptr;
        }

        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    static void on_nats_msg(natsConnection*, natsSubscription*, natsMsg* msg, void* closure)
    {
        auto* self = static_cast<StreamSession*>(closure);
        if (self == nullptr) {
            natsMsg_Destroy(msg);
            return;
        }

        const char* data = natsMsg_GetData(msg);
        const int len = natsMsg_GetDataLength(msg);
        if (data != nullptr && len > 0) {
            self->enqueue(data, static_cast<std::size_t>(len));
        }
        natsMsg_Destroy(msg);
    }

    void enqueue(const char* data, std::size_t size)
    {
        std::vector<char> payload(data, data + size);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(std::move(payload));
            total_ += size;
            if ((total_ - last_log_) >= static_cast<uint64_t>(ntripOptions_GetThroughputLogEveryBytes(ntrip_connection_))) {
                logger_.info(subject_, "Streamed " + std::to_string(total_ / 1024) + "KB");
                last_log_ = total_;
            }
        }
        cv_.notify_one();
    }

    void run()
    {
        while (!stop_ && !parser::g_stop) {
            try {
                connect_and_stream();
                reconnect_delay_ = ntrip_connection_->options().reconnect_min_sec();
            } catch (const std::exception& ex) {
                if (stop_ || parser::g_stop) {
                    break;
                }
                logger_.warn(subject_, ex.what());
                logger_.warn(subject_, "Reconnecting in " + std::to_string(reconnect_delay_) + "s");
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait_for(lk, std::chrono::seconds(reconnect_delay_), [this] { return stop_ || parser::g_stop.load(); });
                reconnect_delay_ = std::min(reconnect_delay_ * 2, ntrip_connection_->options().reconnect_max_sec());
            }
        }
    }

    void connect_and_stream()
    {
        const ntripStatus source_status = ntripConnection_Source(ntrip_connection_);
        if (source_status != NTRIP_OK) {
            throw std::runtime_error("Opening NTRIP source failed: " + std::string(ntripStatus_GetText(source_status)));
        }
        logger_.info(subject_, "Connected to NTRIP source");

        while (!stop_ && !parser::g_stop) {
            std::vector<char> payload;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || parser::g_stop || !queue_.empty(); });
                if (stop_ || parser::g_stop) {
                    return;
                }
                payload = std::move(queue_.front());
                queue_.pop();
            }

            const ntripStatus write_status = ntripConnection_Write(ntrip_connection_, payload.data(), payload.size());
            if (write_status != NTRIP_OK) {
                throw std::runtime_error("Writing to NTRIP failed: " + std::string(ntripStatus_GetText(write_status)));
            }
        }
    }

    ntripConnection* ntrip_connection_ = nullptr;
    natsConnection* nats_connection_ = nullptr;
    Logger& logger_;
    std::string subject_;
    int reconnect_delay_ = 1;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<std::vector<char>> queue_;
    std::thread worker_;
    natsSubscription* subscription_ = nullptr;
    bool stop_ = false;
    uint64_t total_ = 0;
    uint64_t last_log_ = 0;
};

int main(int argc, char* argv[])
{
    Logger logger;

    if (argc == 2) {
        const std::string arg1 = argv[1];
        if (arg1 == "--help" || arg1 == "-h") {
            const std::string prog =
                (argv[0] != nullptr && *argv[0] != '\0') ? argv[0] : "nats2ntrip";
            std::cout
                << "nats2ntrip - NATS to NTRIP bridge\n\n"
                << "Usage:\n"
                << "  " << prog << " <config-file>\n"
                << "  " << prog << " --help\n\n"
                << "Arguments:\n"
                << "  <config-file>   Path to configuration file (e.g. nats2ntrip.conf)\n\n"
                << "Required config format:\n"
                << "  [options]\n"
                << "    read_buffer_bytes=<positive integer>\n"
                << "    reconnect_min_sec=<positive integer>\n"
                << "    reconnect_max_sec=<positive integer>\n"
                << "    throughput_log_kb=<positive integer>\n"
                << "  [nats_sources]\n"
                << "    <nats_url>\n"
                << "    Format: nats://<host>:<port>\n"
                << "  [nats_mountpoints]\n"
                << "    <mountpoint>\n"
                << "  [ntrip_destinations]\n"
                << "    <user>:<pass> <host>:<port>\n"
                << "Notes:\n"
                << "  - Subscription subject is fixed to: " << kSubjectPrefix << "<mountpoint>\n"
                << "  - User-Agent is fixed to: " << kUserAgent << "\n";
            return 0;
        }
    }

    if (argc != 2) {
        const std::string prog = (argv[0] != nullptr && *argv[0] != '\0') ? argv[0] : "nats2ntrip";
        logger.error_sync("main", "Usage: " + prog + " <config-file>");
        logger.error_sync("main", "Use --help to see full documentation.");
        return 1;
    }

    parser::Configuration configuration;
    std::string configuration_error;
    if (!parser::parse_configuration(argv[1], configuration, configuration_error)) {
        logger.error_sync("configuration", configuration_error);
        return 1;
    }

    try {
        std::signal(SIGINT, parser::handle_signal);
        std::signal(SIGTERM, parser::handle_signal);

        natsOptions* raw_nats_options = nullptr;
        const natsStatus create_status = natsOptions_Create(&raw_nats_options);
        if (create_status != NATS_OK) {
            throw std::runtime_error("NATS options create failed: " + std::string(natsStatus_GetText(create_status)));
        }
        std::unique_ptr<natsOptions, decltype(&natsOptions_Destroy)> nat_options(raw_nats_options, natsOptions_Destroy);

        natsOptions_SetDisconnectedCB(nat_options.get(), &parser::nats_on_disconnected, &logger);
        natsOptions_SetReconnectedCB(nat_options.get(), &parser::nats_on_reconnected, &logger);
        natsOptions_SetClosedCB(nat_options.get(), &parser::nats_on_closed, &logger);
        natsOptions_SetErrorHandler(nat_options.get(), &parser::nats_on_async_error, &logger);

        std::vector<const char*> nats_servers;
        nats_servers.reserve(configuration.nats_sources.size());
        for (const auto& nats_source : configuration.nats_sources) {
            nats_servers.push_back(nats_source.server.c_str());
        }
        natsOptions_SetServers(nat_options.get(), nats_servers.data(), static_cast<int>(nats_servers.size()));

        natsConnection* raw_nat_connection = nullptr;
        const natsStatus connect_status = natsConnection_Connect(&raw_nat_connection, nat_options.get());
        if (connect_status != NATS_OK) {
            throw std::runtime_error("NATS connection failed: " + std::string(natsStatus_GetText(connect_status)));
        }
        std::unique_ptr<natsConnection, decltype(&natsConnection_Destroy)> nat_connection(raw_nat_connection, natsConnection_Destroy);

        boost::asio::io_context io;
        std::vector<std::unique_ptr<ntripConnection, decltype(&ntripConnection_Destroy)>> ntrip_servers;
        ntrip_servers.reserve(configuration.nats_mountpoints.size());
        std::vector<std::shared_ptr<StreamSession>> stream_sessions;
        stream_sessions.reserve(configuration.nats_mountpoints.size());
        for (const auto& mountpoint : configuration.nats_mountpoints) {
            ntripOptions* raw_ntrip_options = nullptr;
            const ntripStatus options_status = ntripOptions_Create(&raw_ntrip_options);
            if (options_status != NTRIP_OK) {
                throw std::runtime_error("NTRIP options create failed: " + std::string(ntripStatus_GetText(options_status)));
            }
            std::unique_ptr<ntripOptions, decltype(&ntripOptions_Destroy)> ntrip_options(raw_ntrip_options, ntripOptions_Destroy);

            ntripOptions_SetOptions(ntrip_options.get(), &configuration.options);
            ntripOptions_SetIo(ntrip_options.get(), &io);
            configuration::NtripSource ntrip_destination = configuration.ntrip_destination;
            ntrip_destination.name = mountpoint;
            ntripOptions_SetSource(ntrip_options.get(), &ntrip_destination);

            ntripConnection* raw_ntrip_connection = nullptr;
            const ntripStatus ntrip_connect_status = ntripConnection_Create(&raw_ntrip_connection, ntrip_options.get());
            if (ntrip_connect_status != NTRIP_OK) {
                throw std::runtime_error("NTRIP connection create failed: " + std::string(ntripStatus_GetText(ntrip_connect_status)));
            }
            std::unique_ptr<ntripConnection, decltype(&ntripConnection_Destroy)> ntrip_connection(raw_ntrip_connection, ntripConnection_Destroy);

            auto session = std::make_shared<StreamSession>(ntrip_connection.get(), nat_connection.get(), logger);
            session->start();

            ntrip_servers.push_back(std::move(ntrip_connection));
            stream_sessions.push_back(std::move(session));
        }

        std::unique_lock<std::mutex> lk(parser::g_stop_mtx);
        parser::g_stop_cv.wait(lk, [] { return parser::g_stop.load(); });
        logger.warn("main", "Stopping sessions");
        stream_sessions.clear();
    } catch (const std::exception& ex) {
        logger.error("main", ex.what());
        return 1;
    } catch (...) {
        logger.error("main", "Unexpected error");
        return 1;
    }

    return 0;
}
