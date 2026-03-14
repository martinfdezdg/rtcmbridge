#include <logger/logger.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger::Logger()
{
    worker_ = std::thread([this] { run(); });
}

Logger::~Logger()
{
    stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Logger::info_sync(const std::string& context, const std::string& message)
{
    emit(Level::Info, context, message);
}

void Logger::warn_sync(const std::string& context, const std::string& message)
{
    emit(Level::Warn, context, message);
}

void Logger::error_sync(const std::string& context, const std::string& message)
{
    emit(Level::Error, context, message);
}

void Logger::info(const std::string& context, const std::string& message)
{
    enqueue(Level::Info, context, message);
}

void Logger::warn(const std::string& context, const std::string& message)
{
    enqueue(Level::Warn, context, message);
}

void Logger::error(const std::string& context, const std::string& message)
{
    enqueue(Level::Error, context, message);
}

void Logger::run()
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

void Logger::stop()
{
    stop_ = true;
    cv_.notify_all();
}

std::string Logger::now_timestamp_local()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
    localtime_r(&t, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

const char* Logger::level_name(Level level)
{
    switch (level) {
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

void Logger::emit(Level level,
                  const std::string& context,
                  const std::string& message,
                  const std::string& ts)
{
    std::ostream& os = (level == Level::Error) ? std::cerr : std::cout;
    os << "[" << ts << "]"
       << "[" << level_name(level) << "]"
       << "[" << context << "] "
       << message << "\n";
    os.flush();
}

void Logger::enqueue(Level level, const std::string& context, const std::string& message)
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
