#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include "EventLoop.hpp"

namespace MyServer {

class EventLoopThread {
public:
    EventLoopThread();
    ~EventLoopThread();

    /**
     * @brief 启动底层的线程，并返回该线程中创建的 EventLoop 指针
     */
    EventLoop* startLoop();

private:
    /**
     * @brief 线程真正的执行函数
     * @details 这个函数是在全新的子线程中运行的！
     */
    void threadFunc();

    EventLoop* loop_;             ///< 指向子线程中创建的 EventLoop 对象
    bool exiting_;                ///< 线程是否正在退出
    std::thread thread_;          ///< 底层的 C++ 线程对象

    std::mutex mutex_;            ///< 互斥锁，保护 loop_ 指针的初始化过程
    std::condition_variable cond_;///< 条件变量，用于通知主线程 "EventLoop 已经创建好了！"
};

} // namespace MyServer