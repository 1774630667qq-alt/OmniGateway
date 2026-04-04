#include "EventLoopThread.hpp"
#include "EventLoop.hpp"
#include <mutex>

namespace MyServer {
    EventLoopThread::EventLoopThread() : loop_(nullptr), exiting_(false) {}
    
    EventLoopThread::~EventLoopThread() {
        exiting_ = true;
        if (loop_ != nullptr) {
            loop_->quit();
        }

        if (thread_.joinable()) {
            thread_.join();
        }
    }

    EventLoop* EventLoopThread::startLoop() {
        thread_ = std::thread([this]() {
            this->threadFunc();
        });
        std::unique_lock<std::mutex> lock_(mutex_);
        cond_.wait(lock_, [this]{ return loop_ != nullptr; });
        return loop_;
    }

    void EventLoopThread::threadFunc() {
        EventLoop loop; // 创建 EventLoop
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = &loop;
            cond_.notify_one(); // 唤醒主线程
        }
        loop.loop(); // 进入事件循环
        // loop 销毁后，为防止主线程拿到野指针，将 loop_ 置为 nullptr
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = nullptr;
        }
    }
}