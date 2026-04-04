/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-19 15:20:10
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-29 22:34:11
 * @FilePath: /ServerPractice/include/EventLoop.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <vector>
#include <functional>
#include <mutex>
#include "Timer.hpp"
#include <sys/epoll.h>

namespace MyServer {

class Channel; // 前向声明
class TimerQueue; // 前向声明

/**
 * @brief 事件循环类 (Reactor 模式的发动机)
 * * 它的核心职责是：拥有并管理一个底层的 epoll 实例。
 * 它在一个死循环中不断调用 epoll_wait，一旦有事件发生，
 * 就找出对应触发事件的 Channel，并让 Channel 自己去处理事件。
 * * 它是完全不知道具体的业务逻辑的，也不知道网络报文是什么格式。
 */
class EventLoop {
private:
    int epfd_;      ///< 底层的 epoll 文件描述符
    bool quit_;     ///< 控制死循环是否退出的标志

    // 这是一个预先分配好大小的数组，用来接收 epoll_wait 返回的活跃事件
    std::vector<struct epoll_event> activeEvents_;
    
    // --- 新增：多线程通信组件 ---
    int wakeupFd_;                          ///< 跨线程唤醒的呼叫铃 (eventfd)
    Channel* wakeupChannel_;                ///< 封装呼叫铃的 Channel
    TimerQueue* timerQueue_;                ///< 定时器队列 (管理所有定时器)

    std::mutex mutex_;                      ///< 保护任务队列的互斥锁
    std::vector<std::function<void()>> pendingFunctors_; ///< 出餐台 (任务队列)

    /**
     * @brief 处理呼叫铃响起的事件 (读取 eventfd)
     */
    void handleWakeup();

    /**
     * @brief 执行所有出餐台上的任务
     */
    void doPendingFunctors();

public:
    /**
     * @brief 构造函数：负责调用 epoll_create1(0) 初始化 epfd_，并预分配活跃事件数组
     * @note 如果内核不支持或创建失败，将直接打印错误并退出程序 (exit)。
     */
    EventLoop();

    /**
     * @brief 析构函数：负责 close(epfd_) 释放操作系统的 epoll 资源
     */
    ~EventLoop();

    /**
     * @brief 开启事件分发死循环 (核心发动机)
     * @details 只要 quit_ 为 false，就一直阻塞调用 epoll_wait。
     * 一旦有事件返回，遍历所有活跃事件对应的 Channel 对象，通知其 handleEvent() 进行回调派发。
     */
    void loop();

    /**
     * @brief 更新 Channel 的 epoll 监听状态
     * @details 接收一个 Channel 并根据其内部的 events_ 掩码更新 epoll 树。
     * 内部策略：默认先尝试 EPOLL_CTL_MOD (修改)。如果内核返回 ENOENT 表示该 fd 尚未注册过，
     * 此时便自动退回使用 EPOLL_CTL_ADD (添加) 来注册。
     * @param channel 需要被更新监听状态的 Channel 对象指针
     */
    void updateChannel(Channel* channel);

    // --- 新增：跨线程投递任务的接口 ---
    
    /**
     * @brief 唤醒沉睡在 epoll_wait 的主线程
     */
    void wakeup();

    /**
     * @brief 在指定毫秒数后，执行该段段代码！
     */
    std::shared_ptr<Timer> runAfter(int timeout_ms, TimeoutCallback cb);

    /**
     * @brief 把任务扔进出餐台，并敲响呼叫铃
     * @param cb 需要在主线程执行的函数
     */
    void queueInLoop(std::function<void()> cb);

    void quit() {
        quit_ = true;
    }
};

} // namespace MyServer