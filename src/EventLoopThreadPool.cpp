#include "EventLoopThreadPool.hpp"
#include "EventLoop.hpp"

namespace MyServer {
    EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop) 
        : baseLoop_(baseLoop), started_(false), numThreads_(0), next_(0) {        
    }
    
    void EventLoopThreadPool::start() {
        if (started_) {
            return;
        }
        started_ = true;
        for (int i = 0; i < numThreads_; ++i) {
            threads_.emplace_back(new EventLoopThread());
            loops_.emplace_back(threads_.back()->startLoop());
        }
    }

    EventLoop* EventLoopThreadPool::getNextLoop() {
        if (numThreads_ == 0) {
            return baseLoop_;
        }

        EventLoop* loop = loops_[next_];
        next_ = (next_ + 1) % numThreads_;
        return loop;
    }
}