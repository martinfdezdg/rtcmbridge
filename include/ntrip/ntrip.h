#pragma once

#include <boost/asio.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace configuration {

enum class Section {
    Options,
    NtripSources,
    NatsDestinations
};

struct Options {
    int read_buffer_bytes = 4096;
    int reconnect_min_sec = 2;
    int reconnect_max_sec = 32;
    int throughput_log_every_bytes = 100 * 1024;
};

struct NtripSource {
    std::string user;
    std::string pass;
    std::string host;
    std::string name;
    int port = 0;
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

enum class ntripStatus { NTRIP_OK = 0, NTRIP_ERR = 1 };
static constexpr ntripStatus NTRIP_OK = ntripStatus::NTRIP_OK;
static constexpr ntripStatus NTRIP_ERR = ntripStatus::NTRIP_ERR;

using ntripDataHandler = std::function<void(const char* data, std::size_t n)>;

class ntripOptions {
public:
    ntripOptions();

    void set_options(const configuration::Options& options);
    void set_io(boost::asio::io_context* io);
    void set_source(const configuration::NtripSource& source);

    boost::asio::io_context* io() const;
    const configuration::NtripSource& source() const;

    int read_buffer_bytes() const;
    int reconnect_min_sec() const;
    int reconnect_max_sec() const;
    int throughput_log_every_bytes() const;

private:
    const configuration::Options& get() const;

    configuration::Options options_;
    boost::asio::io_context* io_ = nullptr;
    configuration::NtripSource source_;
};

class ntripConnection {
public:
    using DataHandler = std::function<void(const char* data, std::size_t n)>;

    ntripConnection(boost::asio::io_context& io,
                    ntripOptions options,
                    DataHandler on_data = {});

    const ntripOptions& options() const { return options_; }

private:
    void start();
    void connect();
    void send_request();
    void read();
    void handle_stream_bytes(const char* data, std::size_t n);
    bool response_ok(const std::string& headers) const;
    void reconnect();
    void connect_source();
    void send_source_request();
    ntripStatus write_source(const char* data, std::size_t n);
    void set_consumption_handler(DataHandler consumption_handler);

    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::socket socket_;
    ntripOptions options_;
    DataHandler consumption_handler_;
    std::vector<char> buffer_;
    std::string request_;
    std::string response_buffer_;
    bool response_headers_complete_ = false;
    int delay_ = 1;
    std::unique_ptr<boost::asio::steady_timer> timer_;

    friend ntripStatus ntripOptions_SetHandler(ntripConnection* connection, ntripDataHandler handler);
    friend ntripStatus ntripConnection_Consume(ntripConnection* connection);
    friend ntripStatus ntripConnection_Source(ntripConnection* connection);
    friend ntripStatus ntripConnection_Write(ntripConnection* connection, const char* data, std::size_t n);
};

ntripStatus ntripOptions_Create(ntripOptions** out);
void ntripOptions_Destroy(ntripOptions* options);

ntripStatus ntripOptions_SetOptions(ntripOptions* ntrip_options, const configuration::Options* options);
ntripStatus ntripOptions_SetSource(ntripOptions* ntrip_options, const configuration::NtripSource* source);
ntripStatus ntripOptions_SetIo(ntripOptions* ntrip_options, boost::asio::io_context* io);
ntripStatus ntripOptions_SetHandler(ntripConnection* connection, ntripDataHandler handler);

ntripStatus ntripConnection_Create(ntripConnection** out, ntripOptions* options);
void ntripConnection_Destroy(ntripConnection* connection);

ntripStatus ntripConnection_Consume(ntripConnection* connection);
ntripStatus ntripConnection_Source(ntripConnection* connection);
ntripStatus ntripConnection_Write(ntripConnection* connection, const char* data, std::size_t n);

const char* ntripStatus_GetText(ntripStatus status);
const char* ntripSource_GetText(const ntripConnection* connection);
int ntripOptions_GetThroughputLogEveryBytes(const ntripConnection* connection);
