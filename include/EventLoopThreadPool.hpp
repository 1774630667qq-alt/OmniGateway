#pragma once

#include <vector>
#include <memory>
#include "EventLoopThread.hpp"

namespace MyServer {

class EventLoop;

/**
 * @brief I/O 线程池
 * @details 负责管理多个 EventLoopThread。
 * 它的核心职责是：在服务器启动时，创建指定数量的子线程（每个子线程运行一个独立的 EventLoop）。
 * 当有新的客户端连接到来时，通过轮询（Round-Robin）算法，从池子里挑出一个 EventLoop 分配给这个新连接。
 */
class EventLoopThreadPool {
public:
    /**
     * @brief 构造函数
     * @param baseLoop 主线程的 EventLoop 指针 (也就是总经理，专门负责 Accept 接收新连接的那个)
     */
    EventLoopThreadPool(EventLoop* baseLoop);

    /**
     * @brief 析构函数
     * @details 自动销毁管理的所有 EventLoopThread 对象
     */
    ~EventLoopThreadPool() = default;

    /**
     * @brief 设置线程池中的线程数量
     * @param numThreads 要开启的底层 I/O 线程数量 (通常设置为 CPU 核心数)
     * @note 必须在调用 start() 之前设置！如果设置为 0（默认值），则所有任务都在 baseLoop_ 中执行（退化为单 Reactor 模式）。
     */
    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    /**
     * @brief 启动线程池
     */
    void start();

    /**
     * @brief 获取下一个可用的 EventLoop (核心的负载均衡算法)
     */
    EventLoop* getNextLoop();

private:
    EventLoop* baseLoop_;   ///< 基础循环 (总经理)，当没有子线程时，一切都靠它自己扛
    bool started_;          ///< 状态标志：线程池是否已经启动？
    int numThreads_;        ///< 配置的子线程数量
    int next_;              ///< 轮询算法的游标，记录下一次该分配哪个大堂经理了

    // 这里使用 std::unique_ptr 来管理线程对象的生命周期，利用 RAII 自动析构
    std::vector<std::unique_ptr<EventLoopThread>> threads_; 
    
    // 仅仅保存指向各个子线程内部 EventLoop 的裸指针，用于快速轮询分发。
    // 因为这些 EventLoop 是分配在子线程的栈上的，只要子线程不死，指针就一直有效，不需要释放。
    std::vector<EventLoop*> loops_;                         
};

} // namespace MyServer