#pragma once
#include <functional>
#include <chrono>
#include <memory>

namespace MyServer {

// 使用 C++11 的 chrono 库来搞定高精度时间
using TimeoutCallback = std::function<void()>;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

/**
 * @brief 定时器实体类 (一个秒表)
 */
class Timer {
private:
    TimePoint expiration_;      ///< 绝对到期时间 (比如：2026年3月28日 12:00:30)
    TimeoutCallback callback_;  ///< 到期后要执行的动作 (比如：踢掉连接)
    bool deleted_;              ///< 惰性删除标记 (后面我们在小根堆中会用到)

public:
    Timer(int timeout_ms, TimeoutCallback cb) 
        : callback_(std::move(cb)), deleted_(false) {
        // 计算绝对到期时间：当前时间 + 传进来的毫秒数
        expiration_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    }

    ~Timer() = default;

    // --- 各种 Getter / Setter ---
    TimePoint getExpiration() const { return expiration_; }
    bool isDeleted() const { return deleted_; }
    void setDeleted() { deleted_ = true; }
    
    // 执行超时回调
    void run() {
        if (callback_) {
            callback_();
        }
    }
};

/**
 * @brief 智能指针的比较器（用于让优先队列把最近到期的 Timer 放在最顶上）
 * @details 因为我们要用 std::priority_queue 来管理 shared_ptr<Timer>
 */
struct TimerCmp {
    bool operator()(const std::shared_ptr<Timer>& a, const std::shared_ptr<Timer>& b) const {
        return a->getExpiration() > b->getExpiration();
    }
};

} // namespace MyServer
