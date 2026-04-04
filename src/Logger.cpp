#include "Logger.hpp"
#include "AsyncLogging.hpp"
#include <sys/time.h>
#include <thread>
#include <iostream>

namespace MyServer {

    // ==========================================================
    // 1. 定义全局的异步日志大管家 (使用单例模式的变体：静态局部变量)
    // ==========================================================
    static AsyncLogging* g_asyncLogger = nullptr;

    // 提供一个初始化函数，供 main.cpp 在程序刚启动时调用
    void initGlobalLogger(const std::string& basename) {
        g_asyncLogger = new AsyncLogging(basename);
        g_asyncLogger->start(); // 启动后台扫地僧线程
    }

    // ==========================================================
    // 2. 辅助数组：把枚举转换成字符串前缀
    // ==========================================================
    const char* LogLevelName[4] = {
        "[INFO] ",
        "[WARN] ",
        "[ERROR]",
        "[FATAL]"
    };

    Logger::Logger(const char* file, int line, LogLevel level)
        : level_(level), file_(file), line_(line) {
        
        // 1. 打印时间戳 (我已为你写好这个比较繁琐的系统调用)
        formatTime();

        // 2. 打印日志级别 (利用 level_ 作为索引，去 LogLevelName 数组里拿前缀)
        stream_ << LogLevelName[static_cast<int>(level_)];

        // 3. 打印线程 ID (在高并发下，知道是哪个线程打印的极其重要)
        // 提示：std::this_thread::get_id() 返回的是个对象，你可以把它转成简单的 hash 整数或者直接忽略，为了简单，先追加一个常量字符串如 "[TID] " 占位。
        // 为了简单和高性能，暂时不格式化 std::thread::id，直接用占位符
        stream_ << "[TID] "; // 实际项目中可以缓存哈希值
    }

    Logger::~Logger() {
        // 1. 日志的正文其实在构造到析构之间，已经被业务层通过 << 追加到 stream_ 里了！
        // 2. 打印文件名和行号，并加上换行符！
        stream_ << " - " << file_ << ':' << line_ << '\n';

        // 3. 把 stream_ 里的完整数据，统统喂给全局的大管家 g_asyncLogger！
        if (g_asyncLogger) {
            const LogStream::Buffer& buf = stream_.buffer();
            g_asyncLogger->append(buf.data(), buf.length());
        } else {
            // 防御性编程：如果用户忘了调用 initGlobalLogger，就直接退化成输出到终端屏幕
            const LogStream::Buffer& buf = stream_.buffer();
            std::cout.write(buf.data(), buf.length());
            std::cout.flush();
        }

        // 4. 如果是 FATAL 级别的错误，写完日志后直接让整个程序崩溃退出 (保命)
        if (level_ == LogLevel::FATAL) {
            abort();
        }
    }

    void Logger::formatTime() {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_t seconds = tv.tv_sec;
        struct tm tm_time;
        localtime_r(&seconds, &tm_time);

        char buf[64];
        // 格式化为：2026-03-27 15:30:00.123456 
        int len = snprintf(buf, sizeof(buf), "%4d-%02d-%02d %02d:%02d:%02d.%06ld ",
            tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
            tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
            tv.tv_usec);
        
        stream_.append(buf, len);
    }

} // namespace MyServer