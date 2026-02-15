/*
 * NTRIP Bridge Ultra Escalable
 * Copyright (c) 2026 Martin
 * Licensed for internal / evaluation use.
 */

#include <boost/asio.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <nats/nats.h>

using boost::asio::ip::tcp;
namespace asio = boost::asio;

// ------------------------ Logger ------------------------
class Logger {
public:
    void log(std::string&& msg) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            q_.push(std::move(msg));
        }
        cv_.notify_one();
    }

    void run() {
        std::unique_lock<std::mutex> lk(mtx_);
        while (!stop_) {
            cv_.wait(lk, [&]{ return stop_ || !q_.empty(); });
            while (!q_.empty()) {
                auto msg = std::move(q_.front());
                q_.pop();
                lk.unlock();
                std::cout << msg << std::endl;
                lk.lock();
            }
        }
    }

    void stop() {
        stop_ = true;
        cv_.notify_all();
    }

private:
    std::queue<std::string> q_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};

// ------------------------ Mountpoint ------------------------
struct Mountpoint {
    std::string user, pass;
    std::string host;
    std::string name;
    int port;
};

// ------------------------ Base64 ------------------------
static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& in)
{
    std::string out;
    out.reserve((in.size() * 4) / 3 + 4);
    int val = 0, valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(b64[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// ------------------------ NTRIP Session ------------------------
class NTRIPSession : public std::enable_shared_from_this<NTRIPSession> {
public:
    NTRIPSession(asio::io_context& io, Mountpoint mp,
                 natsConnection* nc, Logger& log)
        : socket_(io),
          mp_(std::move(mp)),
          nats_(nc),
          logger_(log)
    {
        buffer_.resize(4096);
    }

    void start() { connect(); }

private:
    void connect() {
        auto self = shared_from_this();
        tcp::resolver resolver(socket_.get_executor());
        resolver.async_resolve(mp_.host, std::to_string(mp_.port),
            [this, self](auto ec, auto eps) {
                if (!ec) {
                    asio::async_connect(socket_, eps,
                        [this, self](auto ec2, auto) {
                            if (!ec2) send_request();
                            else reconnect();
                        });
                } else reconnect();
            });
    }

    void send_request() {
        std::string auth = mp_.user + ":" + mp_.pass;
        std::string encoded = base64_encode(auth);

        request_ =
            "GET /" + mp_.name + " HTTP/1.1\r\n"
            "User-Agent: NTRIP-CppBridge\r\n"
            "Authorization: Basic " + encoded + "\r\n"
            "Connection: keep-alive\r\n\r\n";

        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(request_),
            [this, self](auto ec, auto) {
                if (!ec) read();
                else reconnect();
            });
    }

    void read() {
        auto self = shared_from_this();
        socket_.async_read_some(asio::buffer(buffer_),
            [this, self](auto ec, std::size_t n) {
                if (!ec) {
                    total_ += n;
                    if (total_ - last_log_ >= 100*1024) {
                        logger_.log("[" + mp_.name + "] " +
                                    std::to_string(total_/1024) + " KB");
                        last_log_ = total_;
                    }
                    std::string subject = "NTRIP." + mp_.name;
                    natsConnection_Publish(nats_, subject.c_str(),
                                           buffer_.data(), n);
                    read();
                } else reconnect();
            });
    }

    void reconnect() {
        socket_.close();
        delay_ = std::min(delay_ * 2, 30);
        logger_.log("[" + mp_.name + "] reconnect in " +
                    std::to_string(delay_) + "s");
        timer_ = std::make_unique<asio::steady_timer>(
            socket_.get_executor(), std::chrono::seconds(delay_));
        auto self = shared_from_this();
        timer_->async_wait([this, self](auto){ connect(); });
    }

    tcp::socket socket_;
    Mountpoint mp_;
    std::vector<char> buffer_;
    std::string request_;
    natsConnection* nats_;
    Logger& logger_;
    uint64_t total_ = 0;
    uint64_t last_log_ = 0;
    int delay_ = 1;
    std::unique_ptr<asio::steady_timer> timer_;
};

// ------------------------ Load mountpoints ------------------------
std::vector<Mountpoint> load_mountpoints(const std::string& file)
{
    std::vector<Mountpoint> v;
    v.reserve(10000);
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        Mountpoint mp;
        auto sp = line.find(' ');
        auto cp = line.find(':');
        auto slash = line.find('/', sp);
        mp.user = line.substr(0, cp);
        mp.pass = line.substr(cp+1, sp-cp-1);
        auto hostport = line.substr(sp+1, slash-sp-1);
        auto colon = hostport.find(':');
        mp.host = hostport.substr(0, colon);
        mp.port = std::stoi(hostport.substr(colon+1));
        mp.name = line.substr(slash+1);
        v.emplace_back(std::move(mp));
    }
    return v;
}

// ------------------------ Main ------------------------
int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: ./ntrip_bridge mountpoints.conf\n";
        return 1;
    }

    Logger logger;
    std::thread log_thread([&]{ logger.run(); });

    natsOptions* opts = nullptr;
    natsConnection* nc = nullptr;
    natsOptions_Create(&opts);

    const char* natsServers[] = {
        "nats://127.0.0.1:4222",
        "nats://127.0.0.1:4223"
    };
    natsOptions_SetServers(opts, natsServers,
                           sizeof(natsServers)/sizeof(natsServers[0]));

    natsStatus s = natsConnection_Connect(&nc, opts);
    if (s != NATS_OK) {
        std::cerr << "NATS connection failed: " << natsStatus_GetText(s) << "\n";
        return 1;
    }

    auto mps = load_mountpoints(argv[1]);
    std::cout << "Loaded " << mps.size() << " mountpoints\n";

    asio::io_context io;
    for (auto& mp : mps)
        std::make_shared<NTRIPSession>(io, mp, nc, logger)->start();

    io.run();

    logger.stop();
    log_thread.join();
    natsConnection_Destroy(nc);
    natsOptions_Destroy(opts);

    return 0;
}
