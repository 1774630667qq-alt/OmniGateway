/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-19 15:19:59
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-19 15:54:55
 * @FilePath: /ServerPractice/include/Channel.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <functional>
#include <sys/epoll.h>
#include <cstdint>
#include <utility>

namespace MyServer {

class EventLoop; // 前向声明，避免头文件循环依赖

/**
 * @brief 通信管道类 (Reactor 模式的核心组件之一)
 * * 它的核心职责是：封装一个文件描述符 (fd) 以及这个 fd 所感兴趣的事件 (events)。
 * 它不直接与操作系统底层的 epoll 打交道，而是把自己的需求告诉 EventLoop。
 * 当事件真正发生时，EventLoop 会调用 Channel 的 handleEvent()，
 * Channel 再根据具体发生的事件，去执行对应的回调函数。
 */
class Channel {
private:
    EventLoop* loop_;       ///< 指向当前 Channel 所属的 EventLoop 大管家
    const int fd_;          ///< 封装的底层的系统文件描述符 (不可变)
    uint32_t events_;       ///< 我【希望】大管家帮我监听的事件掩码 (如 EPOLLIN, EPOLLET)
    uint32_t revents_;      ///< 大管家【告诉我】实际发生的事件掩码 (由 epoll_wait 返回)

    // 核心：使用你之前自己写的 MyStl::function，或者 std::function 都可以。
    // 这里保存的是高层业务逻辑注册进来的代码块。
    std::function<void()> readCallback_;  ///< 发生可读事件时要执行的函数
    std::function<void()> closeCallback_; ///< 发生关闭事件时要执行的函数
    std::function<void()> writeCallback_; ///< 发生可写事件时要执行的函数 

public:
    /**
     * @brief 构造函数：初始化 Channel
     * @param loop 所属的 EventLoop 指针
     * @param fd 需要被封装的文件描述符
     * @note 仅初始化变量，并不会向底层的 epoll 树中注册自身
     */
    Channel(EventLoop* loop, int fd);
    ~Channel() = default;

    /**
     * @brief 核心事件分发器：根据 epoll_wait 返回的具体事件 (revents_) 执行相应回调
     * @details 
     * - EPOLLHUP & !EPOLLIN: 对端关闭连接且无数据可读时，触发 closeCallback_
     * - EPOLLERR: 发生错误时，触发 closeCallback_
     * - EPOLLIN | EPOLLPRI | EPOLLRDHUP: 有数据可读或紧急数据到达时，触发 readCallback_
     * @note 此方法由 EventLoop 在底层 epoll_wait 触发后主动调用。
     */
    void handleEvent();

    /**
     * @brief 注册读事件回调函数 (通常在有新数据到达、或者新连接建立时被触发)
     * @param cb 外部传入的 Lambda 表达式或函数指针
     */
    void setReadCallback(std::function<void()> cb) { readCallback_ = std::move(cb); }

    /**
     * @brief 注册关闭事件回调函数 (用于对端异常断开、发生错误时触发，便于上层清理资源)
     * @param cb 外部传入的 Lambda 表达式或函数指针
     */
    void setCloseCallback(std::function<void()> cb) { closeCallback_ = std::move(cb); }

    /**
     * @brief 注册写事件回调函数 (通常在底层 TCP 发送缓冲区有空位时被触发)
     * @param cb 外部传入的 Lambda 表达式或函数指针
     */
    void setWriteCallback(std::function<void()> cb) { writeCallback_ = std::move(cb); }

    /**
     * @brief 开启对“可写事件”的监听
     * @details 会向事件掩码中添加 EPOLLOUT 标志，并更新到底层 epoll 实例中
     */
    void enableWriting();

    /**
     * @brief 取消对“可写事件”的监听
     * @details 从事件掩码中移除 EPOLLOUT 标志。当数据全部发送完毕后必须调用此方法，防止 CPU 疯狂空转
     */
    void disableWriting();
    
    /**
     * @brief 开启对“可读事件”的监听
     * @details 内部不仅会添加 EPOLLIN 标志，还会开启 EPOLLET (边缘触发) 模式。
     * 修改自身的 events_ 掩码后，会调用所属 EventLoop 的 updateChannel 更新底层的 epoll 实例。
     */
    void enableReading();

    /**
     * @brief 取消所有事件的监听 (优雅注销)
     * @details 将事件掩码清零，并更新底层的 epoll 实例。在对象析构前调用，防止发生幽灵事件。
     */
    void disableAll();

    // --- Getter 和 Setter ---
    int getFd() const { return fd_; }
    uint32_t getEvents() const { return events_; }
    
    /**
     * @brief 设置实际发生的事件 (由 EventLoop 在 epoll_wait 后调用)
     * @param revt epoll 返回的活跃事件掩码
     */
    void setRevents(uint32_t revt) { revents_ = revt; }
};

} // namespace MyServer