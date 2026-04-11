/*
 * @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @Date: 2026-04-05 22:29:51
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-11 15:36:18
 * @FilePath: /OmniGateway/include/Connector.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "EventLoop.hpp"
#include "InetAddress.hpp"
#include <atomic>

namespace MyServer{ 

/**
 * @class Connector
 * @brief 主动连接器：负责在非阻塞模式下发起 TCP 连接
 * * 场景：OmniGateway 作为客户端，主动连接第三方 API 节点。
 * 角色：相当于 TcpServer 中的 Acceptor，但用于主动发起请求。
 */
class Connector : public std::enable_shared_from_this<Connector> {
public:
    // 连接成功后的回调函数：将 fd 移交给上层 HttpClient 以创建 TcpConnection
    using NewConnectionCallback = std::function<void(int sockfd)>;

    Connector(EventLoop* loop, const InetAddress& serverAddr);
    ~Connector();

    /**
     * @brief 开启连接状态机
     * 底层调用 socket() 和 非阻塞 connect()
     */
    void start(); 

    /**
     * @brief 停止连接
     * 清理 Channel 并重置状态
     */
    void stop();

    // 设置连接成功后的“交接”回调
    void setNewConnectionCallback(NewConnectionCallback cb) { 
        newConnectionCallback_ = std::move(cb); 
    }

private:
    /**
     * @brief 核心状态机枚举
     */
    enum StateE { kDisconnected, kConnecting, kConnected };

    /**
     * @brief 内部发起连接的逻辑
     * 1. 创建非阻塞 fd
     * 2. 调用 ::connect()
     * 3. 检查 errno:
     * - EINPROGRESS: 正常，握手进行中，注册 EPOLLOUT 事件
     * - EISCONN: 已经连接成功
     * - 其他: 调用 retry() 进行补偿
     */
    void connect();

    /**
     * @brief 当 Epoll 触发可写事件时的回调 (Connected 判定点)
     * * 重点：可写并不代表成功。
     * 逻辑：调用 getsockopt(fd, SOL_SOCKET, SO_ERROR, ...) 检查错误码。
     * - 若为 0：真正连接成功 -> 调用 newConnectionCallback_。
     * - 若非 0：连接失败 -> 延迟重连。
     */
    void handleWrite();

    /**
     * @brief 错误处理回调
     */
    void handleError();

    /**
     * @brief 指数退避重连算法
     * 防止因服务端宕机导致客户端疯狂发起连接而耗尽本地端口或 CPU
     */
    void retry(int sockfd, int savedErrno = 0);

    // 运行所在的事件循环
    EventLoop* loop_;           
    // 目标地址 (如 api.edgefn.net 的 IP)
    InetAddress serverAddr_;    
    // 当前状态
    std::atomic<StateE> state_; 
    // 观察连接 fd 可写事件的通道
    std::unique_ptr<Channel> channel_; 
    // 成功后的接力回调
    NewConnectionCallback newConnectionCallback_; 

    // 当前重连延迟时间 (ms)
    int retryDelayMs_ = 500;  ///< 初始重连延迟 500ms，每次翻倍，上限 30s
};
}