/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-29 20:51:06
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-29 21:53:04
 * @FilePath: /ServerPractice/include/TimerQueue.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include "Timer.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
#include <queue>
#include <vector>
#include <memory>

namespace MyServer {

/**
 * @brief 定时器大管家 (TimerQueue)
 * @details 
 * 核心架构思想：将 Linux 的 timerfd 与 priority_queue (小根堆) 完美结合。
 * 整个系统【只使用 1 个 timerfd】！
 * 大管家在用户态维护一个“小根堆”，存放这一万个定时器（按到期时间排序，最快到期的在堆顶）。
 * 大管家总是拿出堆顶的到期时间，去设置那唯一的一个 timerfd。
 * 当 timerfd 响铃时，说明堆顶的定时器到期了！大管家就去小根堆里把所有已经到期的定时器全弹出来执行，
 * 然后再拿新的堆顶去重新设置 timerfd。周而复始。
 */
class TimerQueue {
private:
    // 特性：获取最早到期的定时器只需 O(1) 时间，插入新定时器只需 O(logN) 时间。
    using TimerHeap = std::priority_queue<std::shared_ptr<Timer>, 
                                          std::vector<std::shared_ptr<Timer>>, 
                                          TimerCmp>;

    EventLoop* loop_;          ///< 归属的 EventLoop (大堂经理)，大管家必须依附于某一个线程的循环。
    int timerfd_;              ///< 内核提供的时间文件描述符 (整个队列唯一的一颗心脏)。
    Channel* timerChannel_;    ///< 包装 timerfd 的频道，用于向 epoll 注册可读事件。
    TimerHeap timers_;         ///< 在用户态管理所有定时器的小根堆。

    /**
     * @brief 定时器心脏跳动回调 (当 timerfd 触发 EPOLLIN 时被 EventLoop 调用)
     */
    void handleRead();

    /**
     * @brief 重新校准内核的闹钟 (timerfd_settime)
     */
    void resetTimerfd();

public:
    /**
     * @brief 构造函数：创建 timerfd，初始化 Channel，并向 EventLoop 注册
     */
    TimerQueue(EventLoop* loop);
    
    /**
     * @brief 析构函数：关闭 timerfd
     */
    ~TimerQueue();

    /**
     * @brief 添加一个新的定时任务 (例如：心跳包超时剔除、定时发送推送等)
     * @param timeout_ms 从现在起，多少毫秒后触发？
     * @param cb 时间到了之后要执行的回调函数 (Lambda)
     * @return 返回定时器的智能指针。
     */
    std::shared_ptr<Timer> addTimer(int timeout_ms, TimeoutCallback cb);
};

} // namespace MyServer