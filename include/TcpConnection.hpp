/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-20 15:29:42
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-12 15:17:01
 * @FilePath: /ServerPractice/include/TcpConnection.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <functional>
#include <string>
#include <memory>
#include <sys/types.h> // 提供 off_t 类型
#include "Buffer.hpp"

#include <atomic>

typedef struct ssl_st SSL;
namespace MyServer {
class EventLoop;
class Channel;

/**
 * @brief 原子状态机，枚举类型，用于表示连接的状态
 */
enum StateE { 
    kConnecting,    // TCP 正在连接
    kHandshaking,   // TCP 已连上，正在进行 TLS 握手
    kConnected,     // TLS 握手成功，正常加密通信中
    kDisconnecting, // 正在执行关闭流程
    kDisconnected   // 已经彻底断开
};

/**
 * @brief TCP 连接类：代表一个已经建立连接的客户端 (专属酒席)
 * * 它的核心职责是：管理属于这个客户端的 client_fd，以及它专属的 Channel。
 * * 当这个客户端发来消息时，它负责调用 recv() 把数据读出来，
 * * 然后把读到的数据通过回调函数 (messageCallback_) 汇报给大老板 (TcpServer)。
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    /// 连接建立回调类型：TLS 握手成功后触发，参数为当前连接的智能指针
    using ConnectionCallBack = std::function<void(const std::shared_ptr<TcpConnection>&)>;
private:
    EventLoop* loop_;       ///< 大管家 (所属的事件循环)
    SSL* ssl_;              ///< SSL 对象
    int fd_;                ///< 与客户端通信的专属 fd
    int connId_;            ///< 唯一连接 ID（由 TcpServer 分配，防止 fd 复用导致误删）
    Channel* channel_;      ///< 属于这个 fd 的专属通信管道 (服务员)
    Buffer buffer_;         ///< 读数据的缓冲区
    Buffer writeBuffer_;    ///< 写数据的缓冲区

    std::atomic<StateE> state_; ///< 原子状态机，用于表示连接的状态

    // --- 给上层大老板 (TcpServer) 留的汇报接口 ---
    
    ///< 当收到客人发来的消息时，触发此回调。参数1是当前连接的智能指针，参数2是收到的字符串
    std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)> messageCallback_;
    
    ///< 当客人断开连接时，触发此回调。参数是当前连接的智能指针
    std::function<void(const std::shared_ptr<TcpConnection>&)> closeCallback_;

    ///< 当 TLS 握手成功、连接完全建立时触发此回调
    ConnectionCallBack connectionCallback_;



    /**
     * @brief 处理连接关闭事件
     */
    void handleClose();
public:
    /**
     * @brief 构造函数：接管客户端文件描述符及 SSL 会话对象，并初始化其专属 Channel
     * @param loop 所在的 EventLoop 实例
     * @param fd 客户端已连接的非阻塞套接字文件描述符
     * @param ssl SSL 会话对象指针（从 serverCtx_ 或 clientCtx_ 派生）
     */
    TcpConnection(EventLoop* loop, int fd, SSL* ssl);
    
    /**
     * @brief 析构函数：按顺序释放 SSL 会话资源、销毁内部 Channel，并关闭底层 TCP 套接字
     * @details
     *   1. SSL_shutdown()  —— 向对端发送 TLS close_notify，有序关闭密文通道
     *   2. SSL_free()      —— 释放会话密鑰、BIO 等内部资源（不会 close fd）
     *   3. channel负责关闭 / delete channel_
     *   4. ::close(fd_)    —— 彻底关闭底层 TCP 连接
     */
    ~TcpConnection();

    /**
     * @brief 核心方法：处理套接字可读事件
     * @details 因为使用的是 EPOLLET (边缘触发) 模式，所以在该函数内部，
     * 会在一个死循环中不断调用 SSL_read() 读取解密后的流量数据，直到返回 WANT_READ 为止。
     * - 读到数据时，将触发 messageCallback_ 向上层抛出。
     * - 遇到 SSL_ERROR_ZERO_RETURN 或不可恢复的错误时，将触发 closeCallback_ 并安全地向上层通知注销自己。
     */
    void handleRead(); 

    /**
     * @brief 处理套接字可写事件
     * @details 当底层 TCP 写缓冲区有空闲空间时（由 epoll 触发 EPOLLOUT），该函数会被调用。
     * 它负责将应用层写缓冲区 (writeBuffer_) 中积压的数据通过 SSL_write() 加密后发送给对端。
     * 如果所有数据都发送完毕，会取消对 EPOLLOUT 事件的监听，防止 CPU 空转。
     */
    void handleWrite();

    // --- 注册回调的 Setter ---
    void setMessageCallback(std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)> cb) {
        messageCallback_ = std::move(cb);
    }
    void setCloseCallback(std::function<void(const std::shared_ptr<TcpConnection>&)> cb) {
        closeCallback_ = std::move(cb);
    }
    void setConnectionCallback(ConnectionCallBack cb) {
        connectionCallback_ = std::move(cb);
    }

    /**
     * @brief 通过 SSL 加密通道发送数据到对端 (非阻塞异步发送逻辑)
     * @param msg 要发送的字符串数据
     * @details
     * 1. 如果当前的写缓冲区 (writeBuffer_) 是空的，则直接尝试调用 SSL_write() 加密发送。
     * 2. 如果 SSL_write 返回 SSL_ERROR_WANT_WRITE，或写缓冲区本身就有积压的数据，
     *    就把剩余未发送的数据追加到写缓冲区中，并向 epoll 注册 EPOLLOUT 可写事件。
     *    由后续触发的 handleWrite() 负责继续发送。
     */
    void send(const std::string& msg);

    /**
     * @brief 获取当前连接所绑定的客户端文件描述符
     * @return 客户端的 Socket FD
     */
    int getFd() const { return fd_; }

    /**
     * @brief 设置唯一连接 ID
     */
    void setConnId(int id) { connId_ = id; }

    /**
     * @brief 获取唯一连接 ID
     */
    int getConnId() const { return connId_; }

    /**
     * @brief 获取当前连接所属的事件循环 (EventLoop) 大管家
     * @return 指向所在的 EventLoop 对象的指针
     */
    EventLoop* getLoop() { return loop_; };

    /**
     * @brief 在 IO 线程中完成连接的初始化：注册 epoll 读事件并自动发起 TLS 握手
     * @details
     *   必须在连接所属的 ioLoop 线程中调用，不可在主线程中直接调用。
     *   内部执行顺序：
     *   1. channel_->enableReading() —— 注册 EPOLLIN 事件
     *   2. doTlsHandshake()           —— 自动开始 TLS 握手（服务端/客户端均适用）
     */
    void connectEstablished();



    /**
     * @brief 零拷贝发送文件接口
     * @param filepath 要发送的本地文件绝对或相对路径
     */
    void sendFile(const std::string& filepath);

    /**
     * @brief 强制关闭连接
     * @details 立即触发 closeCallback_ 并注销自己
     */
    void forceClose();

    /**
     * @brief 发起非阻塞 TLS 握手（自动适配客户端/服务端角色）
     * @signature void doTlsHandshake();
     * @details
     *   [职责]
     *   在 TCP 三次握手完成后，对持有 SSL 对象的连接发起 TLS 握手。
     *   内部调用 SSL_do_handshake()，该函数会根据 SSL_CTX 的来源
     *   自动选择客户端握手（SSL_connect 行为）或服务端握手（SSL_accept 行为）。
     * 
     *   [非阻塞握手流程]
     *   由于底层 fd 是非阻塞的，SSL_do_handshake() 通常不会一次性完成，
     *   而是返回 SSL_ERROR_WANT_READ 或 SSL_ERROR_WANT_WRITE。
     *   此时应根据返回值注册相应的 epoll 事件，
     *   在后续的 handleRead() / handleWrite() 中检测到 kHandshaking 状态时
     *   重新调用本方法继续握手，直到握手成功后将状态切换为 kConnected。
     * 
     *   [调用时机]
     *   由 connectEstablished() 在 IO 线程中自动调用，无需外部手动触发。
     */
    void doTlsHandshake();

    void setState(StateE state) { state_ = state; }

    StateE getState() const { return state_; }
};

} // namespace MyServer