#include "EventLoop.hpp"
#include "Channel.hpp"
#include "Logger.hpp"
#include <sys/epoll.h>
#include <sys/eventfd.h> // 提供 eventfd 函数
#include <unistd.h> // 提供 close 函数
#include "TimerQueue.hpp"

namespace MyServer
{
    /**
     * @brief 辅助函数创建一个 eventfd 用于跨线程唤醒
     * @return 成功返回 eventfd 的文件描述符，失败则直接退出程序
     */
    static int createEventFd() {
        /**
         * @brief 创建一个 eventfd 对象，专门用于跨线程的事件通知机制 (系统调用)
         * @param initval 初始计数器的值，通常设为 0
         * @param flags   操作标志位（支持使用按位或 | 组合使用）：
         *                - EFD_NONBLOCK: 设置为非阻塞模式。如果读取时计数器为 0，不会阻塞当前线程，而是立即返回 EAGAIN 错误。
         *                - EFD_CLOEXEC:  (Close-on-exec) 在当前进程执行 exec() 系列系统调用派生子进程时，自动关闭该文件描述符，防止其泄露给子进程。
         *                - EFD_SEMAPHORE: (从 Linux 2.6.30 开始支持) 提供类似信号量的语义。如果设置此标志，每次 read() 时不论计数器多大，只会将其减 1 并返回 1；若不设置（默认），read() 会一次性读出计数器的所有累加值，并将其清零。
         * @return 成功返回新的文件描述符，失败返回 -1 并设置 errno
         */
        int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (evtfd < 0) {
            LOG_FATAL << "Failed in eventfd";
        }
        return evtfd;
    }

    EventLoop::EventLoop() : epfd_(-1), quit_(false), activeEvents_(1024) {
        // 创建 epoll 实例
        epfd_ = epoll_create1(0);
        if (epfd_ == -1) {
            LOG_FATAL << "Epoll 创建失败!";
        }

        // 创建 eventfd 用于跨线程唤醒
        wakeupFd_ = createEventFd();
        wakeupChannel_ = new Channel(this, wakeupFd_);

        // 绑定 eventfd 的读事件回调
        wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleWakeup, this));
        wakeupChannel_->enableReading(); // 启用读事件监听
        timerQueue_ = new TimerQueue(this); // 初始化定时器队列
    }

    EventLoop::~EventLoop() {
        delete timerQueue_;
        wakeupChannel_->disableAll(); 
        delete wakeupChannel_;
        close(wakeupFd_);
        if (epfd_ != -1) {
            close(epfd_);
        }
    }

    void EventLoop::wakeup() {
        uint64_t one = 1;
        /**
         * @brief 向文件描述符写入数据 (系统调用，用于触发 eventfd 的可读事件)
         * @param fd    文件描述符 (此处的 wakeupFd_)
         * @param buf   指向要写入数据的缓冲区的指针 (必须是 8 字节的无符号整数 uint64_t)
         * @param count 要写入的字节数 (通常为 sizeof(uint64_t))
         * @return 成功返回实际写入的字节数，失败返回 -1 并设置 errno
         */
        ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
        if (n != sizeof(one)) {
            LOG_ERROR << "EventLoop 唤醒失败!";
        }
    }

    void EventLoop::handleWakeup() {
        uint64_t one;
        /**
         * @brief 从文件描述符读取数据 (系统调用，用于消耗掉 eventfd 的可读事件)
         * @param fd    文件描述符 (此处的 wakeupFd_)
         * @param buf   指向存放读取数据缓冲区的指针 (用来承接那 8 个字节)
         * @param count 要读取的最大字节数 (通常为 sizeof(uint64_t))
         * @return 成功返回实际读取的字节数，失败返回 -1 并设置 errno
         */
        ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
        if (n != sizeof(one)) {
            LOG_ERROR << "EventLoop 处理唤醒事件失败!";
        }
    }

    void EventLoop::queueInLoop(std::function<void()> cb) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingFunctors_.push_back(std::move(cb));
        }
        wakeup(); // 唤醒主线程，让它来执行出餐台上的任务
    }

    void EventLoop::doPendingFunctors() {
        std::vector<std::function<void()>> functors;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            functors.swap(pendingFunctors_); // 交换出餐台上的任务，避免长时间持锁
        }
        for (const auto& functor : functors) {
            functor(); // 执行每一个任务
        }
    }

    void EventLoop::loop() {
        while (!quit_) {
            int nfds = epoll_wait(epfd_, activeEvents_.data(), static_cast<int>(activeEvents_.size()), -1);
            if (nfds == -1) {
                LOG_ERROR << "Epoll 等待事件失败!";
                continue; // 出错了，但我们不想退出整个服务器，所以继续循环
            }

            for (int i = 0; i < nfds; ++i) {
                Channel* channel = static_cast<Channel*>(activeEvents_[i].data.ptr);
                channel->setRevents(activeEvents_[i].events); // 把实际发生的事件告诉 Channel
                channel->handleEvent(); // 让 Channel 自己去处理事件
            }
            // 在退出循环前，执行所有出餐台上的任务，确保没有遗漏
            doPendingFunctors();
        }
    }

    void EventLoop::updateChannel(Channel* channel) {
        int fd = channel->getFd();
        uint32_t events = channel->getEvents();

        struct epoll_event ev;
        ev.data.ptr = channel; 
        // 把 Channel 的指针存到 epoll_event 的data.ptr 中，这样 epoll_wait 醒来时就能找到对应的 Channel 对象
        ev.events = events; // 关注 Channel 想监听的事件

        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
            if (errno == ENOENT) {
                // 如果是因为这个 fd 还没有被注册过，那么就添加它
                if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
                    LOG_ERROR << "Epoll 添加 Channel 失败!";
                }
            } else {
                LOG_ERROR << "Epoll 修改 Channel 失败!";
            }
        }
    }

    std::shared_ptr<Timer> EventLoop::runAfter(int timeout_ms, TimeoutCallback cb) {
        return timerQueue_->addTimer(timeout_ms, std::move(cb));
    }
}