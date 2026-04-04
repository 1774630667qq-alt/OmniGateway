/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-20 16:06:48
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-31 14:14:35
 * @FilePath: /ServerPractice/include/TcpServer.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <unordered_map>
#include <functional>
#include <memory>
#include "Buffer.hpp"
#include "EventLoopThreadPool.hpp"
namespace MyServer {

class EventLoop;
class Acceptor;
class TcpConnection;

/**
 * @brief 服务器主类：向开发者暴露的最外层接口 (大老板)
 * * 它负责把底层所有的零件 (EventLoop, Acceptor, TcpConnection) 组装起来。
 * * 开发者只需要实例化这个类，并设置好“收到消息怎么处理”的回调即可。
 */
class TcpServer {
private:
    EventLoop* loop_;       ///< 大管家 (外部传进来的)
    Acceptor* acceptor_;    ///< 迎宾员 (专门接收新连接)
    
    ///< 账本：记录当前餐厅里所有的客人。Key 是 fd，Value 是对应的 TcpConnection 对象
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_; 

    ///< 业务逻辑回调：让外部开发者决定，收到消息后到底该干嘛？(比如做回声、做HTTP解析等)
    std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)> onMessageCallback_;
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    int nextConnId_;  ///< 单调递增的连接 ID，用于唯一标识每个连接（防止 fd 复用导致误删）

public:
    /**
     * @brief 构造函数：初始化服务器实例
     * @param loop 外部实例化的 EventLoop 指针
     * @param port 服务器需要监听绑定的端口号
     * @details 内部会自动创建负责接收连接的 Acceptor 迎宾员，并绑定新连接的调度回调(newConnection)。
     */
    TcpServer(EventLoop* loop, int port);

    /**
     * @brief 析构函数：释放 Acceptor 并销毁账本(connections_)中所有尚未断开的客户端连接资源。
     */
    ~TcpServer();

    /**
     * @brief 启动服务器：通知内部 Acceptor 开始 listen 并注册到 epoll 中
     */
    void start();

    /**
     * @brief 迎宾员接到新客人后，触发此函数
     * @details 内部会为新连接实例化 TcpConnection 对象，并将其注册到 connections_ 账本中。
     * 另外会给该连接分配对应的读事件回调(onMessageCallback_) 和 退出回调(removeConnection)。
     * @param fd 客户端的文件描述符
     */
    void newConnection(int fd);

    /**
     * @brief 客人离开或连接异常时，触发此函数
     * @details 核心职责是：在底层抛出销毁请求时，从账本(connections_)中将该连接擦除，并销毁该 TcpConnection 对象的堆内存。
     * @param conn 需要销毁的客户端连接智能指针
     */
    void removeConnection(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 在主线程中安全地执行连接移除操作
     * @details 由 removeConnection 投递到主线程执行，保证 connections_ map 只在主线程被访问
     */
    void removeConnectionInLoop(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 业务逻辑暴露接口：注册当收到客户端消息时触发的回调
     * @param cb 回调签名，提供对应的连接智能指针 `std::shared_ptr<TcpConnection>` 以及解码出的纯文本信息 `const std::string&`
     */
    void setOnMessageCallback(std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)> cb) {
        onMessageCallback_ = std::move(cb);
    }

    /**
     * @brief 向外暴露的设置线程池数量的接口
     * @param numThreads 线程池数量
     */
    void setThreadNum(int numThreads) {
        threadPool_->setThreadNum(numThreads);
    }
};

} // namespace MyServer