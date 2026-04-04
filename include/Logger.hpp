/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-27 17:59:24
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-02 19:27:34
 * @FilePath: /ServerPractice/include/Logger.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include "LogStream.hpp"
#include <string>

namespace MyServer {

// 全局日志初始化函数 (必须在程序开始时调用一次)
void initGlobalLogger(const std::string& basename);

// 1. 定义日志级别
enum class LogLevel {
    INFO,
    WARNING,
    ERROR,
    FATAL // 致命错误，打印完直接让程序 abort() 退出
};

/**
 * @brief 面向用户的日志包装类
 * @details 它的生命周期极短！通常作为临时对象存在于一行代码中。
 * 构造时：记录时间戳、文件、行号等头部信息。
 * 析构时：自动把 LogStream 里的数据扔给全局的 AsyncLogging！
 */
class Logger {
public:
    /**
     * @brief 构造函数
     * @param file 源码文件名 (通过 __FILE__ 宏获取)
     * @param line 源码行号 (通过 __LINE__ 宏获取)
     * @param level 日志级别
     */
    Logger(const char* file, int line, LogLevel level);
    
    /**
     * @brief 析构函数
     */
    ~Logger();

    // 暴露内部的流，让业务层可以用 << 追加数据
    LogStream& stream() { return stream_; }

private:
    LogStream stream_;   ///< 内部的字符串加工厂
    LogLevel level_;     ///< 当前日志的级别
    const char* file_;   ///< 记录日志所在的文件
    int line_;           ///< 记录日志所在的行号

    // 辅助函数：格式化时间戳
    void formatTime();
};

// ==========================================================
// ⭐ 终极魔法：宏定义
// ==========================================================
// 原理剖析：当业务层写下 LOG_INFO << "hello"; 时
// 编译器会展开为：Logger(__FILE__, __LINE__, LogLevel::INFO).stream() << "hello";
// 这会创建一个匿名的 Logger 临时对象。当这行代码执行完（遇到分号），临时对象被销毁，触发析构函数！

#define LOG_INFO    MyServer::Logger(__FILE__, __LINE__, MyServer::LogLevel::INFO).stream()
#define LOG_WARNING MyServer::Logger(__FILE__, __LINE__, MyServer::LogLevel::WARNING).stream()
#define LOG_ERROR   MyServer::Logger(__FILE__, __LINE__, MyServer::LogLevel::ERROR).stream()
#define LOG_FATAL   MyServer::Logger(__FILE__, __LINE__, MyServer::LogLevel::FATAL).stream()

} // namespace MyServer