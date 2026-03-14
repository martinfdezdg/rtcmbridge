#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

class Logger {
public:
    enum class Level { Info, Warn, Error };

    Logger();

    ~Logger();

    void info_sync(const std::string& context, const std::string& message);

    void warn_sync(const std::string& context, const std::string& message);

    void error_sync(const std::string& context, const std::string& message);

    void info(const std::string& context, const std::string& message);

    void warn(const std::string& context, const std::string& message);

    void error(const std::string& context, const std::string& message);

private:
    struct Entry {
        Level level = Level::Info;
        std::string context;
        std::string message;
    };

    void run();

    void stop();

    static std::string now_timestamp_local();

    static const char* level_name(Level level);

    static void emit(Level level,
                     const std::string& context,
                     const std::string& message,
                     const std::string& ts = now_timestamp_local());

    void enqueue(Level level, const std::string& context, const std::string& message);

    std::queue<Entry> q_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
