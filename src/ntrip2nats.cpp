/*
 * ntrip2nats.cpp
 * NTRIP -> NATS bridge
 * Copyright (c) 2026 Martin
 * Licensed for internal / evaluation use.
 */

#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <nats/nats.h>
#include <ntrip/ntrip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <logger/logger.h>

static constexpr const char* kSubjectPrefix = "NTRIP.";
static constexpr const char* kUserAgent = "NTRIP-CppBridge";

namespace {
namespace parser {

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

bool parse_section_header(const std::string& line, configuration::Section& section, std::string& error)
{
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        error = "Invalid section header";
        return false;
    }
    
    const std::string section_line = boost::algorithm::to_lower_copy(line.substr(1, line.size() - 2));
    if (section_line == "options") {
        section = configuration::Section::Options;
        return true;
    }
    if (section_line == "nats_destinations") {
        section = configuration::Section::NatsDestinations;
        return true;
    }
    if (section_line == "ntrip_sources") {
        section = configuration::Section::NtripSources;
        return true;
    }

    error = "Unknown section '" + section_line + "'";
    return false;
}

bool parse_options_body_line(const std::string& line,
                             configuration::Configuration& configuration,
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

bool parse_nats_destinations_body_line(const std::string& line,
                                       configuration::Configuration& configuration,
                                       std::string& error)
{
    const std::string prefix = "nats://";
    if (line.substr(0, prefix.size()) != prefix) {
        error = "Invalid NATS destination format";
        return false;
    }
    const auto host_limit = line.find(':', prefix.size());
    if (host_limit == std::string::npos || host_limit == 0) {
        error = "Invalid NATS destination format: missing ':'";
        return false;
    }
    int port = 0;
    if (!parse_positive_int(line.substr(host_limit + 1), port)) {
        error = "Invalid NATS destination format: invalid port";
        return false;
    }

    configuration::NatsDestination nats_destination;
    nats_destination.server = prefix + line.substr(prefix.size(), host_limit - prefix.size()) + ":" + std::to_string(port);

    configuration.nats_destinations.push_back(std::move(nats_destination));
    return true;
}

// - Credentials may not contain spaces (we split on the first space).
// - Username may not contain ':' (we split on the first ':').
// - Host must be in host:port form.
bool parse_ntrip_sources_body_line(const std::string& line,
                                   configuration::Configuration& configuration,
                                   std::string& error)
{
    const auto user_limit = line.find(':');
    if (user_limit == std::string::npos || user_limit == 0) {
        error = "Invalid NTRIP source format: missing ':'";
        return false;
    }
    const auto pass_limit = line.find(' ', user_limit + 1);
    if (pass_limit == std::string::npos || pass_limit == user_limit + 1) {
        error = "Invalid NTRIP source format: missing space";
        return false;
    }
    const auto host_limit = line.find(':', pass_limit + 1);
    if (host_limit == std::string::npos || host_limit == pass_limit + 1) {
        error = "Invalid NTRIP source format: missing ':'";
        return false;
    }
    const auto port_limit = line.find('/', host_limit + 1);
    if (port_limit == std::string::npos || port_limit == host_limit + 1) {
        error = "Invalid NTRIP source format: missing '/'";
        return false;
    }

    configuration::NtripSource ntrip_source;
    ntrip_source.user = line.substr(0, user_limit);
    ntrip_source.pass = line.substr(user_limit + 1, pass_limit - user_limit - 1);
    ntrip_source.host = line.substr(pass_limit + 1, host_limit - pass_limit - 1);
    if (!parse_positive_int(line.substr(host_limit + 1, port_limit - host_limit - 1), ntrip_source.port)) {
        error = "Invalid NTRIP source format: invalid port";
        return false;
    }
    ntrip_source.name = line.substr(port_limit + 1);

    configuration.ntrip_sources.push_back(std::move(ntrip_source));
    return true;
}

bool parse_configuration(const std::string& path, configuration::Configuration& configuration, std::string& error)
{
    std::ifstream in(path);
    if (!in) {
        error = "Cannot open configuration file " + path;
        return false;
    }

    // Default initial section.
    configuration::Section section = configuration::Section::Options;

    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed_line = boost::algorithm::trim_copy(line);

        if (!trimmed_line.empty() && trimmed_line.front() != '#') {
            // Section header line.
            if (trimmed_line.front() == '[') {
                if (!parse_section_header(trimmed_line, section, error)) {
                    return false;
                }
            // Section body lines.
            } else {
                switch (section) {
                    case configuration::Section::Options:
                        if (!parse_options_body_line(trimmed_line, configuration, error)) {
                            return false;
                        }
                        break;
                    case configuration::Section::NatsDestinations:
                        if (!parse_nats_destinations_body_line(trimmed_line, configuration, error)) {
                            return false;
                        }
                        break;
                    case configuration::Section::NtripSources:
                        if (!parse_ntrip_sources_body_line(trimmed_line, configuration, error)) {
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
    if (configuration.nats_destinations.empty()) {
        error = "No NATS destinations configured";
        return false;
    }
    if (configuration.ntrip_sources.empty()) {
        error = "No NTRIP sources configured";
        return false;
    }

    return true;
}  // namespace parser
}  // namespace

}

namespace {

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

}  // namespace

class StreamSession : public std::enable_shared_from_this<StreamSession> {
public:
    StreamSession(ntripConnection* ntrip_connection,
                  natsConnection* nats_connection,
                  Logger& logger)
        : nats_connection_(nats_connection),
          ntrip_connection_(ntrip_connection),
          logger_(logger)
    {
        subject = std::string(kSubjectPrefix) + std::string(ntripSource_GetText(ntrip_connection_));
    }

    void start()
    {
        logger_.info(subject, "Starting session");
        ntripOptions_SetHandler(ntrip_connection_, [this](const char* data, std::size_t n) { handle_stream(data, n); });
        const ntripStatus consume_status = ntripConnection_Consume(ntrip_connection_);
        if (consume_status != NTRIP_OK) {
            throw std::runtime_error("Consuming from NTRIP failed: " + std::string(ntripStatus_GetText(consume_status)));
        }
    }

private:
    natsConnection* nats_connection_;
    ntripConnection* ntrip_connection_;
    Logger& logger_;

    std::string subject;
    uint64_t total_ = 0;
    uint64_t last_log_ = 0;

    void handle_stream(const char* data, std::size_t n)
    {
        total_ += n;
        if ((total_ - last_log_) >= ntripOptions_GetThroughputLogEveryBytes(ntrip_connection_)) {
            logger_.info(subject, "Streamed " + std::to_string(total_ / 1024) + "KB");
            last_log_ = total_;
        }

        const natsStatus publish_status = natsConnection_Publish(nats_connection_, subject.c_str(), data, static_cast<int>(n));
        if (publish_status != NATS_OK) {
            logger_.warn_sync(subject, "Publishing to NATS failed: " + std::string(natsStatus_GetText(publish_status)));
        }
    }
};

int main(int argc, char* argv[])
{
    Logger logger;

    if (argc == 2) {
        const std::string arg1 = argv[1];
        if (arg1 == "--help" || arg1 == "-h") {
            const std::string prog =
                (argv[0] != nullptr && *argv[0] != '\0') ? argv[0] : "ntrip2nats";
            std::cout
                << "ntrip2nats - NTRIP to NATS bridge\n\n"
                << "Usage:\n"
                << "  " << prog << " <config-file>\n"
                << "  " << prog << " --help\n\n"
                << "Arguments:\n"
                << "  <config-file>   Path to configuration file (e.g. ntrip2nats.conf)\n\n"
                << "Required config format:\n"
                << "  [options]\n"
                << "    read_buffer_bytes=<positive integer>\n"
                << "    reconnect_min_sec=<positive integer>\n"
                << "    reconnect_max_sec=<positive integer>\n"
                << "    throughput_log_kb=<positive integer>\n"
                << "  [nats_destinations]\n"
                << "    <nats_url>\n"
                << "    Format: nats://<host>:<port>\n"
                << "  [ntrip_sources]\n"
                << "    <user>:<pass> <host>:<port>/<mountpoint>\n"
                << "Notes:\n"
                << "  - Subject prefix is fixed to: " << kSubjectPrefix << "\n"
                << "  - User-Agent is fixed to: " << kUserAgent << "\n";
            return 0;
        }
    }

    if (argc != 2) {
        const std::string prog = (argv[0] != nullptr && *argv[0] != '\0') ? argv[0] : "ntrip2nats";
        logger.error_sync("main", "Usage: " + prog + " <config-file>");
        logger.error_sync("main", "Use --help to see full documentation.");
        return 1;
    }

    configuration::Configuration configuration;
    std::string configuration_error;
    if (!parser::parse_configuration(argv[1], configuration, configuration_error)) {
        logger.error_sync("configuration", configuration_error);
        return 1;
    }

    try {
        natsOptions* raw_nats_options = nullptr;
        const natsStatus create_status = natsOptions_Create(&raw_nats_options);
        if (create_status != NATS_OK) {
            throw std::runtime_error("NATS options create failed: " + std::string(natsStatus_GetText(create_status)));
        }
        std::unique_ptr<natsOptions, decltype(&natsOptions_Destroy)> nat_options(raw_nats_options, natsOptions_Destroy);
        
        natsOptions_SetDisconnectedCB(nat_options.get(), &nats_on_disconnected, &logger);
        natsOptions_SetReconnectedCB(nat_options.get(), &nats_on_reconnected, &logger);
        natsOptions_SetClosedCB(nat_options.get(), &nats_on_closed, &logger);
        natsOptions_SetErrorHandler(nat_options.get(), &nats_on_async_error, &logger);
        std::vector<const char*> nats_servers;
        nats_servers.reserve(configuration.nats_destinations.size());
        for (const auto& nats_destination : configuration.nats_destinations) {
            nats_servers.push_back(nats_destination.server.c_str());
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
        ntrip_servers.reserve(configuration.ntrip_sources.size());
        std::vector<std::shared_ptr<StreamSession>> stream_sessions;
        stream_sessions.reserve(configuration.ntrip_sources.size());
        for (const auto& ntrip_source : configuration.ntrip_sources) {
            ntripOptions* raw_ntrip_options = nullptr;
            const ntripStatus options_status = ntripOptions_Create(&raw_ntrip_options);
            if (options_status != NTRIP_OK) {
                throw std::runtime_error("NTRIP options create failed: " + std::string(ntripStatus_GetText(options_status)));
            }
            std::unique_ptr<ntripOptions, decltype(&ntripOptions_Destroy)> ntrip_options(raw_ntrip_options, ntripOptions_Destroy);

            ntripOptions_SetOptions(ntrip_options.get(), &configuration.options);
            ntripOptions_SetIo(ntrip_options.get(), &io);
            ntripOptions_SetSource(ntrip_options.get(), &ntrip_source);

            ntripConnection* raw_ntrip_connection = nullptr;
            const ntripStatus connect_status = ntripConnection_Create(&raw_ntrip_connection, ntrip_options.get());
            if (connect_status != NTRIP_OK) {
                throw std::runtime_error("NTRIP connection create failed: " + std::string(ntripStatus_GetText(connect_status)));
            }
            std::unique_ptr<ntripConnection, decltype(&ntripConnection_Destroy)> ntrip_connection(raw_ntrip_connection, ntripConnection_Destroy);
            
            auto session = std::make_shared<StreamSession>(ntrip_connection.get(), nat_connection.get(), logger);
            session->start();
            
            ntrip_servers.push_back(std::move(ntrip_connection));
            stream_sessions.push_back(std::move(session));
        }
        io.run();

        logger.warn("main", "Sessions stopped");
    } catch (const std::exception& ex) {
        logger.error("main", ex.what());
        return 1;
    } catch (...) {
        logger.error("main", "Unexpected error");
        return 1;
    }

    return 0;
}
