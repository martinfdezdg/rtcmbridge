/*
 * ntrip2nats.cpp
 * NTRIP -> NATS bridge
 * Copyright (c) 2026 Martin
 * Licensed for internal / evaluation use.
 */

#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <nats/nats.h>
#include <openssl/evp.h>

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

static constexpr const char* kSubjectPrefix = "NTRIP.";
static constexpr const char* kUserAgent = "NTRIP-CppBridge";
static constexpr const char* kConnectionHeader = "keep-alive";

namespace configuration {

enum class Section {
    Options,
    NtripSources,
    NatsDestinations
};

struct Options {
    int read_buffer_bytes = 4096;
    int reconnect_initial_sec = 2;
    int reconnect_max_sec = 32;
    int throughput_log_every_bytes = 100 * 1024;
};

struct NtripSource {
    std::string user;
    std::string pass;
    std::string host;
    std::string name;
    int port;
};

struct NatsDestination {
    std::string server;
};

struct Configuration {
    Options options;
    std::vector<NtripSource> ntrip_sources;
    std::vector<NatsDestination> nats_destinations;
};

}

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

    if (key == "reconnect_ini_sec") {
        if (!parse_positive_int(value, configuration.options.reconnect_initial_sec)) {
            error = "Invalid option value 'reconnect_initial_sec'";
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

    if (configuration.options.reconnect_initial_sec > configuration.options.reconnect_max_sec) {
        error = "Value of reconnect_initial_sec cannot be greater than reconnect_max_sec";
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
}

}

namespace encoder {

std::string base64(const std::string& in)
{
    if (in.empty()) {
        return {};
    }

    std::string out(4 * ((in.size() + 2) / 3), '\0');
    auto* in_ptr = reinterpret_cast<const unsigned char*>(in.data());
    auto* out_ptr = reinterpret_cast<unsigned char*>(out.data());
    const int written = EVP_EncodeBlock(out_ptr, in_ptr, static_cast<int>(in.size()));
    if (written <= 0) {
        return {};
    }

    out.resize(written);
    return out;
}

}

class Logger {
public:
    enum class Level { Info, Warn, Error };

    Logger() : worker_([this] { run(); }) {}

    ~Logger() {
        stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void info_sync(const std::string& context, const std::string& message)
    {
        emit(Level::Info, context, message);
    }

    void warn_sync(const std::string& context, const std::string& message)
    {
        emit(Level::Warn, context, message);
    }

    void error_sync(const std::string& context, const std::string& message)
    {
        emit(Level::Error, context, message);
    }

    void info(const std::string& context, const std::string& message)
    {
        enqueue(Level::Info, context, message);
    }

    void warn(const std::string& context, const std::string& message)
    {
        enqueue(Level::Warn, context, message);
    }

    void error(const std::string& context, const std::string& message)
    {
        enqueue(Level::Error, context, message);
    }

    void run()
    {
        std::unique_lock<std::mutex> lk(mtx_);
        while (true) {
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            while (!q_.empty()) {
                Entry e = std::move(q_.front());
                q_.pop();
                lk.unlock();
                emit(e.level, e.context, e.message);
                lk.lock();
            }
            if (stop_) break;
        }
    }

    void stop()
    {
        stop_ = true;
        cv_.notify_all();
    }

private:
    struct Entry {
        Level level = Level::Info;
        std::string context;
        std::string message;
    };

    static std::string now_timestamp_local()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);

        std::tm tm{};

        localtime_r(&t, &tm);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    static const char* level_name(Level level)
    {
        switch (level) {
            case Level::Info: return "INFO";
            case Level::Warn: return "WARN";
            case Level::Error: return "ERROR";
        }
        return "INFO";
    }

    static void emit(Level level,
                     const std::string& context,
                     const std::string& message,
                     const std::string& ts = now_timestamp_local())
    {
        std::ostream& os = (level == Level::Error) ? std::cerr : std::cout;
        os << "[" << ts << "]"
           << "[" << level_name(level) << "]"
           << "[" << context << "] "
           << message << "\n";
        os.flush();
    }

    void enqueue(Level level, const std::string& context, const std::string& message)
    {
        Entry e;
        e.level = level;
        e.context = context;
        e.message = message;

        {
            std::lock_guard<std::mutex> lk(mtx_);
            q_.push(std::move(e));
        }
        cv_.notify_one();
    }

    std::thread worker_;
    std::queue<Entry> q_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};

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
    logger->warn("nats", url.empty() ? "disconnected" : ("disconnected from " + url));
}

void nats_on_reconnected(natsConnection* nc, void* closure)
{
    auto* logger = static_cast<Logger*>(closure);
    if (logger == nullptr) return;
    const std::string url = nats_connected_url(nc);
    logger->info("nats", url.empty() ? "reconnected" : ("reconnected to " + url));
}

void nats_on_closed(natsConnection* /*nc*/, void* closure)
{
    auto* logger = static_cast<Logger*>(closure);
    if (logger == nullptr) return;
    logger->error("nats", "connection closed (no more reconnect attempts)");
}

void nats_on_async_error(natsConnection* /*nc*/,
                         natsSubscription* /*sub*/,
                         natsStatus err,
                         void* closure)
{
    auto* logger = static_cast<Logger*>(closure);
    if (logger == nullptr) return;
    logger->warn("nats", "async error: " + std::string(natsStatus_GetText(err)));
}

}  // namespace

class Ntrip2NatsSession : public std::enable_shared_from_this<Ntrip2NatsSession> {
public:
    Ntrip2NatsSession(boost::asio::io_context& io,
                      configuration::Options options,
                      configuration::NtripSource ntrip_source,
                      natsConnection* nats_connection,
                      Logger& logger)
        : resolver_(io),
          socket_(io),
          options_(std::move(options)),
          nats_connection_(nats_connection),
          ntrip_source_(std::move(ntrip_source)),
          logger_(logger),
          delay_(options_.reconnect_initial_sec)
    {
        buffer_.resize(options_.read_buffer_bytes);
    }

    // Starts the options
    void start()
    {
        logger_.info(ntrip_source_.name, "starting options");
        connect();
    }

private:
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    configuration::Options options_;
    natsConnection* nats_connection_;
    configuration::NtripSource ntrip_source_;
    std::vector<char> buffer_;
    std::string request_;
    Logger& logger_;
    uint64_t total_ = 0;
    uint64_t last_log_ = 0;
    int delay_ = 1;
    std::unique_ptr<boost::asio::steady_timer> timer_;

    void connect()
    {
        auto self = shared_from_this();
        resolver_.async_resolve(ntrip_source_.host, std::to_string(ntrip_source_.port), [this, self](auto ec, auto eps) {
            if (!ec) {
                boost::asio::async_connect(socket_, eps, [this, self](auto ec2, auto) {
                    if (!ec2) {
                        send_request();
                    } else {
                        reconnect();
                    }
                });
            } else {
                reconnect();
            }
        });
    }

    void send_request()
    {
        std::string credentials = ntrip_source_.user + ":" + ntrip_source_.pass;
        std::string encoded_credentials = encoder::base64(credentials);
        if (!credentials.empty() && encoded_credentials.empty()) {
            logger_.error(ntrip_source_.name, "Credentials encoding failed");
            reconnect();
            return;
        }

        request_ =
            "GET /" + ntrip_source_.name + " HTTP/1.1\r\n"
            "User-Agent: " + std::string(kUserAgent) + "\r\n"
            "Authorization: Basic " + encoded_credentials + "\r\n"
            "Connection: " + std::string(kConnectionHeader) + "\r\n\r\n";

        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(request_), [this, self](auto ec, auto) {
            if (!ec) {
                read();
            } else {
                reconnect();
            }
        });
    }

    void read()
    {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(buffer_), [this, self](auto ec, std::size_t n) {
            if (!ec) {
                // IMPORTANT: this implementation does not parse/strip the initial HTTP response headers.
                // If the caster sends headers (typical), the first publish may include them unless the
                // server starts streaming RTCM immediately after the handshake. If you see "HTTP/1.1"
                // bytes on the NATS side, add a one-time header read until "\\r\\n\\r\\n".
                total_ += n;

                if ((total_ - last_log_) >= options_.throughput_log_every_bytes) {
                    logger_.info(ntrip_source_.name, "streamed=" + std::to_string(total_ / 1024U) + "KB");
                    last_log_ = total_;
                }

                // Subject naming is intentionally deterministic for consumers:
                // NTRIP.<mountpoint>. If mountpoint names can contain '.' or other separators in your
                // environment, consider normalizing/sanitizing them to avoid subscription surprises.
                const std::string subject = std::string(kSubjectPrefix) + ntrip_source_.name;
                const natsStatus s =
                    natsConnection_Publish(nats_connection_, subject.c_str(), buffer_.data(), static_cast<int>(n));
                if (s != NATS_OK) {
                    logger_.warn_sync(ntrip_source_.name, "nats publish failed: " + std::string(natsStatus_GetText(s)));
                }

                read();
            } else {
                reconnect();
            }
        });
    }

    void reconnect()
    {
        socket_.close();
        delay_ = std::min(delay_ * 2, options_.reconnect_max_sec);
        logger_.warn(ntrip_source_.name, "reconnect in " + std::to_string(delay_) + "s");

        timer_ = std::make_unique<boost::asio::steady_timer>(socket_.get_executor(), std::chrono::seconds(delay_));
        auto self = shared_from_this();
        timer_->async_wait([this, self](auto) { connect(); });
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
                << "    reconnect_ini_sec=<positive integer>\n"
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
        // Creamos instancia de configuración de NATS
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
        for (const auto& ntrip_source : configuration.ntrip_sources) {
            std::make_shared<Ntrip2NatsSession>(io, configuration.options, ntrip_source, nat_connection.get(), logger)->start();
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
