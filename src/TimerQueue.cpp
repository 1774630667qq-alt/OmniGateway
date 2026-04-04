#include "TimerQueue.hpp"
#include <sys/timerfd.h>
#include "Logger.hpp"
#include <chrono>
#include <unistd.h>

namespace MyServer {
    TimerQueue::TimerQueue(EventLoop* loop) : loop_(loop) { 
        // 创建 timerfd
        /**
         * @brief 创建一个定时器文件描述符 (系统调用)
         * @param clockid 指定定时器所使用的时钟类型：
         *                - CLOCK_REALTIME: 系统范围内的实时时钟。如果系统时间被手动修改，定时器也会受影响。
         *                - CLOCK_MONOTONIC: 单调递增时钟，从系统启动后开始计时。不受系统时间修改的影响，是作为相对时间定时器的最佳选择。
         * @param flags   操作标志位（支持使用按位或 | 组合使用）：
         *                - TFD_NONBLOCK: 设置为非阻塞模式。如果读取时定时器尚未到期触发，不会阻塞当前线程，而是立即返回 EAGAIN 错误。
         *                - TFD_CLOEXEC:  (Close-on-exec) 在当前进程执行 exec() 系列系统调用派生子进程时，自动关闭该文件描述符，防止其泄露给子进程。
         * @return 成功返回新的文件描述符，失败返回 -1 并设置 errno
         */
        timerfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timerfd_ == -1) {
            LOG_FATAL << "Failed to create timerfd";
        }

        // 初始化 Channel，绑定 timerfd 和 EventLoop
        timerChannel_ = new Channel(loop_, timerfd_);
        // 设置 timerfd 可读事件的回调函数
        timerChannel_->setReadCallback(std::bind(&TimerQueue::handleRead, this));
        // 启用 timerfd 的读事件监听
        timerChannel_->enableReading();
    }

    TimerQueue::~TimerQueue() {
        // 在析构前关闭所有事件监听，防止在 epoll 中残留幽灵事件
        timerChannel_->disableAll();
        // 关闭 timerfd
        ::close(timerfd_);
        delete timerChannel_;
    }

    std::shared_ptr<Timer> TimerQueue::addTimer(int timeout_ms, TimeoutCallback ch) {
        // 获取当前时间戳，加上持续时间，得到绝对到期时间
        auto timer = std::make_shared<Timer>(timeout_ms, std::move(ch));
        bool early_change = timers_.empty() || timer->getExpiration() < timers_.top()->getExpiration();
        timers_.push(timer);
        if (early_change) {
            resetTimerfd();
        }
        return timer;
    }

    void TimerQueue::resetTimerfd() {
        if (timers_.empty()) {
            return;
        }
        auto now = std::chrono::steady_clock::now();
        auto next_expire = timers_.top()->getExpiration();
        // 终极修复：使用微秒精度，避免因为小于 0 被赋为 0 而导致底层定时器彻底停止
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(next_expire - now);
        if (duration.count() <= 0) {
            duration = std::chrono::microseconds(1); // 保证至少 1 微秒，防止内核误以为要关闭定时器
        }

        /**
         * @brief 定义定时器的时间参数结构体 (itimerspec)
         * @details 该结构体包含两个 timespec 类型的成员，分别控制定时器的初始触发时间和后续的周期触发时间。
         *          （每个 timespec 结构体内都包含 tv_sec 秒 和 tv_nsec 纳秒）
         * @param it_value    首次到期时间 (即首次触发的延时时间，或系统的绝对时间，取决于 timerfd_settime 的 flags 标志位)。
         *                    - 特殊值/作用：如果 it_value 设为 0 (即 tv_sec=0 且 tv_nsec=0)，则意味着【停止/解除】该定时器。
         * @param it_interval 周期性定时器的触发间隔时间。
         *                    - 特殊值/作用：如果 it_interval 设为 0，则表示该定时器为【一次性】定时器 (one-shot)，触发后不再自动重启。
         */
        struct itimerspec new_value;
        new_value.it_value.tv_sec = duration.count() / 1000000;
        new_value.it_value.tv_nsec = (duration.count() % 1000000) * 1000;
        new_value.it_interval.tv_sec = 0; // 不需要周期性触发
        new_value.it_interval.tv_nsec = 0;

        /**
         * @brief 启动或停止由文件描述符 fd 指定的定时器 (系统调用)
         * @param fd      由 timerfd_create 返回的定时器文件描述符 (此处为 timerfd_)
         * @param flags   控制定时器设置方式的标志位：
         *                - 0: 相对时间，即从调用该函数时刻起，经过 new_value.it_value 指定的时间后到期。
         *                - TFD_TIMER_ABSTIME: 绝对时间，即当系统的时钟到达 new_value.it_value 指定的绝对时间时到期。
         * @param new_value 指向 itimerspec 结构体的指针，指定定时器的初始到期时间 (it_value) 和周期性间隔时间 (it_interval)。若 it_value 设为 0，则直接停止该定时器。
         * @param old_value 如果不为 nullptr，则用于返回定时器在被修改之前的旧设置值。此处不需要，传 nullptr。
         * @return 成功返回 0，失败返回 -1 并设置 errno
         */
        if (::timerfd_settime(timerfd_, 0, &new_value, nullptr) == -1) {
            LOG_ERROR << "Failed to set timerfd";
        }
    }

    void TimerQueue::handleRead() {
        uint64_t expirations;
        ssize_t n = ::read(timerfd_, &expirations, sizeof(expirations));
        if (n != sizeof(expirations)) {
            LOG_ERROR << "Failed to read timerfd";
            return;
        }

        auto now = std::chrono::steady_clock::now();
        while (!timers_.empty()) {
            auto timer = timers_.top();
            if (timer->getExpiration() <= now) {
                timers_.pop();
                if (!timer->isDeleted()) {
                    timer->run();
                }
            } else {
                break;
            }
        }
        resetTimerfd();
    }
} // namespace MyServer