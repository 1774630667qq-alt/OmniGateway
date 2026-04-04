/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-19 16:38:40
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-19 16:38:43
 * @FilePath: /ServerPractice/include/Accept.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <functional>

namespace MyServer {

class EventLoop;
class Channel;

/**
 * @brief 迎宾员类：专门负责处理新客户端的连接请求
 */
class Acceptor {
private:
    EventLoop* loop_;       ///< 大管家
    int listen_fd_;         ///< 监听套接字 (大门)
    Channel* acceptChannel_;///< 专门为大门分配的通信管道
    int port_;              ///< 监听的端口号

    // 核心回调：当迎宾员成功接到新客人后，他不知道怎么安排客人，
    // 他必须通过这个回调函数，把新客人的 fd 交给餐厅大老板 (TcpServer) 去处理。
    std::function<void(int)> newConnectionCallback_; 

public:
    /**
     * @brief 构造函数：初始化服务器监听大门
     * @details 
     * 1. 创建 IPv4 TCP 监听套接字
     * 2. 开启 SO_REUSEADDR 端口复用，防止重启时端口占用报错
     * 3. 绑定(bind)传入的本地端口
     * 4. 设置监听套接字为非阻塞(O_NONBLOCK)模式
     * 5. 创建并初始化监听套接字专属的 acceptChannel_
     * @param loop 所属的 EventLoop 大管家
     * @param port 需监听的本机端口
     */
    Acceptor(EventLoop* loop, int port);

    /**
     * @brief 析构函数：清理监听套接字(close)及专属通信管道的内存
     */
    ~Acceptor();

    /**
     * @brief 供上层大老板调用的回调注册接口
     * @param cb 接收新连接套接字描述符 `int client_fd` 的回调函数
     */
    void setNewConnectionCallback(std::function<void(int)> cb) {
        newConnectionCallback_ = std::move(cb);
    }

    /**
     * @brief 接收新连接的可读事件核心逻辑 (由 acceptChannel_ 触发)
     * @details 由于是并发及非阻塞模式，会在 while(true) 中循环执行 accept() 获取所有 pending 的连接，
     * 并将每一个新客户端套接字设为非阻塞模式，直到返回 EAGAIN 才结束本轮读取，最后触发 newConnectionCallback_ 向上抛出。
     */
    void handleRead();

    /**
     * @brief 开始迎客：调用 listen() 开启监听，并把 acceptChannel_ 的读事件挂载到底层 epoll 树中
     */
    void listen();
};

} // namespace MyServer