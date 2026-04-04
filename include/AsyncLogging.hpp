/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-27 16:29:50
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-28 16:15:51
 * @FilePath: /ServerPractice/include/AsyncLogging.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include "LogStream.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace MyServer {

/**
 * @brief 异步日志的核心：双缓冲大管家
 */
class AsyncLogging {
public:
    // 这里我们使用 4MB 的超大缓冲区来装载无数条 4KB 的小日志
    using Buffer = FixedBuffer<kLargeBuffer>; 
    // 使用 unique_ptr 管理大块内存，防止内存泄漏，且转移所有权极快
    using BufferPtr = std::unique_ptr<Buffer>;
    // 存放已经写满、等待后台刷盘的缓冲区队列
    using BufferVector = std::vector<BufferPtr>;

private:
    std::string basename_;             ///< 日志文件名的前缀 (如 "MyServer_Log")
    bool running_;                     ///< 后台线程运行标志
    std::thread thread_;               ///< 后台专门写磁盘的“扫地僧”线程
    
    std::mutex mutex_;                 ///< 保护下面所有缓冲区的互斥锁
    std::condition_variable cond_;     ///< 用于前台唤醒后台的条件变量

    BufferPtr currentBuffer_;          ///< 当前正在使用的桶 (桶 A)
    BufferPtr nextBuffer_;             ///< 备用的桶 (桶 B)
    BufferVector buffers_;             ///< 已经装满，等着被倒进磁盘的桶队列

    /**
     * @brief 后台线程的核心工作函数 (极其烧脑的交换逻辑)
     */
    void threadFunc();

public:
    AsyncLogging(const std::string& basename = "MyServer");
    ~AsyncLogging();

    // 启动和停止后台线程
    void start() {
        running_ = true;
        // 启动后台线程，执行 threadFunc
        thread_ = std::thread(&AsyncLogging::threadFunc, this);
    }

    void stop() {
        running_ = false;
        cond_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /**
     * @brief 供前端业务线程调用的写日志接口
     * @param logline 单条完整日志的首地址 (来自 LogStream)
     * @param len 单条日志的长度
     */
    void append(const char* logline, int len);
};

} // namespace MyServer