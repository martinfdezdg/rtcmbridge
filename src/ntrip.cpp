#include <ntrip/ntrip.h>

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kUserAgent = "NTRIP-CppBridge";
constexpr const char* kConnectionHeader = "keep-alive";

std::string encode_chunk(const char* data, std::size_t n)
{
    std::ostringstream out;
    out << std::hex << n << "\r\n";
    out.write(data, static_cast<std::streamsize>(n));
    out << "\r\n";
    return out.str();
}

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
    response_headers_complete_ = false;
    response_buffer_.clear();

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
            handle_stream_bytes(buffer_.data(), n);
            read();
        } else {
            reconnect();
        }
    });
}

void ntripConnection::handle_stream_bytes(const char* data, std::size_t n)
{
    if (data == nullptr || n == 0) {
        return;
    }

    if (response_headers_complete_) {
        if (consumption_handler_) {
            consumption_handler_(data, n);
        }
        return;
    }

    response_buffer_.append(data, n);

    const std::size_t headers_end = response_buffer_.find("\r\n\r\n");
    if (headers_end == std::string::npos) {
        return;
    }

    const std::string headers = response_buffer_.substr(0, headers_end + 4);
    if (!response_ok(headers)) {
        reconnect();
        return;
    }

    response_headers_complete_ = true;
    const std::size_t payload_offset = headers_end + 4;
    if (payload_offset < response_buffer_.size() && consumption_handler_) {
        consumption_handler_(response_buffer_.data() + payload_offset, response_buffer_.size() - payload_offset);
    }
    response_buffer_.clear();
}

bool ntripConnection::response_ok(const std::string& headers) const
{
    const std::size_t line_end = headers.find("\r\n");
    const std::string status_line = headers.substr(0, line_end);
    return status_line.find(" 200 ") != std::string::npos || status_line.rfind("ICY 200", 0) == 0;
}

void ntripConnection::reconnect()
{
    response_headers_complete_ = false;
    response_buffer_.clear();

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

void ntripConnection::connect_source()
{
    boost::system::error_code ignored_ec;
    resolver_.cancel();
    socket_.cancel(ignored_ec);
    socket_.close(ignored_ec);

    const auto endpoints = resolver_.resolve(options_.source().host, std::to_string(options_.source().port));
    boost::asio::connect(socket_, endpoints);
}

void ntripConnection::send_source_request()
{
    const std::string credentials = options_.source().user + ":" + options_.source().pass;
    const std::string encoded_credentials = base64(credentials);
    if (!credentials.empty() && encoded_credentials.empty()) {
        throw std::runtime_error("Failed to encode NTRIP credentials");
    }

    request_ =
        "POST /" + options_.source().name + " HTTP/1.1\r\n"
        "Host: " + options_.source().host + ":" + std::to_string(options_.source().port) + "\r\n"
        "User-Agent: " + std::string(kUserAgent) + "\r\n"
        "Ntrip-Version: Ntrip/2.0\r\n"
        "Authorization: Basic " + encoded_credentials + "\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: " + std::string(kConnectionHeader) + "\r\n\r\n";

    boost::asio::write(socket_, boost::asio::buffer(request_));
}

void ntripConnection::read_source_response()
{
    response_headers_complete_ = false;
    response_buffer_.clear();

    boost::asio::read_until(socket_, boost::asio::dynamic_buffer(response_buffer_), "\r\n\r\n");

    const std::size_t headers_end = response_buffer_.find("\r\n\r\n");
    if (headers_end == std::string::npos) {
        throw std::runtime_error("Invalid NTRIP source response");
    }

    const std::string headers = response_buffer_.substr(0, headers_end + 4);
    if (!response_ok(headers)) {
        throw std::runtime_error("NTRIP source rejected upload");
    }

    response_headers_complete_ = true;
    response_buffer_.clear();
}

ntripStatus ntripConnection::write_source(const char* data, std::size_t n)
{
    if (data == nullptr || n == 0) {
        return ntripStatus::NTRIP_ERR;
    }

    const std::string chunk = encode_chunk(data, n);
    boost::asio::write(socket_, boost::asio::buffer(chunk));
    return ntripStatus::NTRIP_OK;
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

ntripStatus ntripConnection_Source(ntripConnection* connection)
{
    if (connection == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }

    try {
        connection->connect_source();
        connection->send_source_request();
        connection->read_source_response();
        return ntripStatus::NTRIP_OK;
    } catch (...) {
        return ntripStatus::NTRIP_ERR;
    }
}

ntripStatus ntripConnection_Write(ntripConnection* connection, const char* data, std::size_t n)
{
    if (connection == nullptr) {
        return ntripStatus::NTRIP_ERR;
    }

    try {
        return connection->write_source(data, n);
    } catch (...) {
        return ntripStatus::NTRIP_ERR;
    }
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
