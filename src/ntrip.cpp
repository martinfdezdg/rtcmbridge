#include <ntrip/ntrip.h>

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace {

constexpr const char* kUserAgent = "NTRIP-CppBridge";
constexpr const char* kConnectionHeader = "keep-alive";

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

ntripOptions::ntripOptions() = default;

void ntripOptions::set_options(const configuration::Options& options)
{
    options_ = options;
}

void ntripOptions::set_io(boost::asio::io_context* io)
{
    io_ = io;
}

void ntripOptions::set_source(const configuration::NtripSource& source)
{
    source_ = source;
}

const configuration::Options& ntripOptions::get() const
{
    return options_;
}

boost::asio::io_context* ntripOptions::io() const
{
    return io_;
}

const configuration::NtripSource& ntripOptions::source() const
{
    return source_;
}

int ntripOptions::read_buffer_bytes() const
{
    return options_.read_buffer_bytes;
}

int ntripOptions::reconnect_min_sec() const
{
    return options_.reconnect_min_sec;
}

int ntripOptions::reconnect_max_sec() const
{
    return options_.reconnect_max_sec;
}

int ntripOptions::throughput_log_every_bytes() const
{
    return options_.throughput_log_every_bytes;
}

ntripStatus ntripOptions_Create(ntripOptions** out)
{
    if (out == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }
    try {
        *out = new ntripOptions();
        return (*out != nullptr) ? ntripStatus::NTRIP_OK : ntripStatus::NTRIP_ERR;
    } catch (...) {
        *out = nullptr;
        return ntripStatus::NTRIP_ERR;
    }
}

void ntripOptions_Destroy(ntripOptions* options)
{
    delete options;
}

ntripStatus ntripOptions_SetOptions(ntripOptions* ntrip_options, const configuration::Options* options)
{
    if (ntrip_options == nullptr || options == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }
    ntrip_options->set_options(*options);
    return ntripStatus::NTRIP_OK;
}

ntripStatus ntripOptions_SetIo(ntripOptions* ntrip_options, boost::asio::io_context* io)
{
    if (ntrip_options == nullptr || io == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }
    ntrip_options->set_io(io);
    return ntripStatus::NTRIP_OK;
}

ntripStatus ntripOptions_SetSource(ntripOptions* ntrip_options, const configuration::NtripSource* source)
{
    if (ntrip_options == nullptr || source == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }
    ntrip_options->set_source(*source);
    return ntripStatus::NTRIP_OK;
}

ntripConnection::ntripConnection(boost::asio::io_context& io,
                                 ntripOptions options,
                                 DataHandler consumption_handler)
    : resolver_(io),
      socket_(io),
      options_(std::move(options)),
      consumption_handler_(std::move(consumption_handler)),
      delay_(options_.reconnect_min_sec())
{
    buffer_.resize(options_.read_buffer_bytes());
}

void ntripConnection::start()
{
    connect();
}

void ntripConnection::connect()
{
    resolver_.async_resolve(options_.source().host, std::to_string(options_.source().port), [this](auto ec, auto eps) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (!ec) {
            boost::asio::async_connect(socket_, eps, [this](auto ec2, auto) {
                if (ec2 == boost::asio::error::operation_aborted) {
                    return;
                }
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

void ntripConnection::send_request()
{
    const std::string credentials = options_.source().user + ":" + options_.source().pass;
    const std::string encoded_credentials = base64(credentials);
    if (!credentials.empty() && encoded_credentials.empty()) {
        reconnect();
        return;
    }

    request_ =
        "GET /" + options_.source().name + " HTTP/1.1\r\n"
                                           "User-Agent: " +
        std::string(kUserAgent) + "\r\n"
                                  "Authorization: Basic " +
        encoded_credentials + "\r\n"
                              "Connection: " +
        std::string(kConnectionHeader) + "\r\n\r\n";

    boost::asio::async_write(socket_, boost::asio::buffer(request_), [this](auto ec, auto) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (!ec) {
            read();
        } else {
            reconnect();
        }
    });
}

void ntripConnection::read()
{
    socket_.async_read_some(boost::asio::buffer(buffer_), [this](auto ec, std::size_t n) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (!ec) {
            if (consumption_handler_) {
                consumption_handler_(buffer_.data(), n);
            }
            read();
        } else {
            reconnect();
        }
    });
}

void ntripConnection::reconnect()
{
    boost::system::error_code ignored_ec;
    resolver_.cancel();
    if (timer_) {
        timer_->cancel();
    }
    socket_.cancel(ignored_ec);
    socket_.close(ignored_ec);
    delay_ = std::min(delay_ * 2, options_.reconnect_max_sec());

    timer_ = std::make_unique<boost::asio::steady_timer>(socket_.get_executor(), std::chrono::seconds(delay_));
    timer_->async_wait([this](auto ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        connect();
    });
}

void ntripConnection::set_consumption_handler(DataHandler consumption_handler)
{
    consumption_handler_ = std::move(consumption_handler);
}

ntripStatus ntripConnection_Consume(ntripConnection* connection)
{
    if (connection == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }
    connection->start();
    return ntripStatus::NTRIP_OK;
}

ntripStatus ntripConnection_Create(ntripConnection** out, ntripOptions* options)
{
    if (out == nullptr || options == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }
    boost::asio::io_context* io = options->io();
    if (io == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }
    try {
        *out = new ntripConnection(*io, *options);
        return (*out != nullptr) ? ntripStatus::NTRIP_OK : ntripStatus::NTRIP_ERR;
    } catch (...) {
        *out = nullptr;
        return ntripStatus::NTRIP_ERR;
    }
}

void ntripConnection_Destroy(ntripConnection* connection)
{
    delete connection;
}

const char* ntripStatus_GetText(ntripStatus status)
{
    switch (status) {
        case ntripStatus::NTRIP_OK:
            return "NTRIP_OK";
        case ntripStatus::NTRIP_ERR:
            return "NTRIP_ERR";
    }
    return "NTRIP_ERR";
}

const char* ntripSource_GetText(const ntripConnection* connection)
{
    return connection->options().source().name.c_str();
}

int ntripOptions_GetThroughputLogEveryBytes(const ntripConnection* connection)
{
    if (connection == nullptr) {
        return 0;
    }
    return connection->options().throughput_log_every_bytes();
}

ntripStatus ntripOptions_SetHandler(ntripConnection* connection, ntripDataHandler handler)
{
    if (connection == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }
    connection->set_consumption_handler(std::move(handler));
    return ntripStatus::NTRIP_OK;
}
