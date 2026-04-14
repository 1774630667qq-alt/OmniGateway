/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-20 16:06:42
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-14 19:54:10
 * @FilePath: /ServerPractice/src/TcpServer.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "TcpServer.hpp"
#include "EventLoop.hpp"
#include "Accept.hpp"
#include "Logger.hpp"
#include "TcpConnection.hpp"
#include <openssl/ssl.h>
#include "SSLManager.hpp"
#include <openssl/err.h>
#include <unistd.h>

namespace MyServer {

TcpServer::TcpServer(EventLoop* loop, int port)
    : loop_(loop), acceptor_(nullptr), threadPool_(new EventLoopThreadPool(loop)), nextConnId_(0) {
    acceptor_ = new Acceptor(loop_, port);
    
    // 告诉迎宾员：接到新客人后，把 fd 交给我的 newConnection 方法！
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1)
    );
}

TcpServer::~TcpServer() {
    delete acceptor_;
    // 清理所有尚未关闭的连接
    connections_.clear();
}

void TcpServer::start() {
    threadPool_->start();
    acceptor_->listen();
}

void TcpServer::newConnection(int fd) {
    EventLoop* ioLoop = threadPool_->getNextLoop();
    int connId = nextConnId_++;  // 分配唯一连接 ID（主线程执行，无需加锁）

    SSL* ssl = SSL_new(SSLManager::getServerCtx());
    if (!ssl) {
        ERR_print_errors_fp(stderr);
        LOG_ERROR << "SSL_new 失败，无法为前端连接创建 SSL 对象 fd=" << fd;
        ::close(fd);
        return;
    }

    if (!SSL_set_fd(ssl, fd)) {
        ERR_print_errors_fp(stderr);
        LOG_ERROR << "SSL_set_fd 失败 fd=" << fd;
        SSL_free(ssl);
        ::close(fd);
        return;
    }

    /**
     * @brief 将 SSL 对象显式设置为服务端模式（等待 ClientHello 的一方）
     * @signature void SSL_set_accept_state(SSL *ssl);
     * @param ssl 目标 SSL 会话对象
     * @details
     *   [职责]
     *   SSL_new() 从 SSL_CTX 派生出 SSL 对象后，其内部的握手函数指针 (handshake_func)
     *   默认为 NULL，SSL_do_handshake() 无法判断应执行客户端握手还是服务端握手，
     *   导致 "connection type not set" 错误。
     *
     *   [与 HttpClient 的对比]
     *   HttpClient::onConnection() 中调用了 SSL_set_connect_state() 设置客户端模式。
     *   服务端对应地必须调用本函数设置服务端模式，两者缺一不可。
     *
     *   [工作流程]
     *   本函数将 ssl->handshake_func 设置为 ssl->method->ssl_accept，
     *   后续 SSL_do_handshake() 即可正确地等待客户端发来 ClientHello 并执行服务端握手。
     */
    SSL_set_accept_state(ssl);

    // 1. 创建一个新的 TcpConnection 对象，传入 SSL 指针使其成为加密连接

    // 【关键修复】：使用 std::shared_ptr 的自定义删除器！
    // 这样无论哪个线程（例如 HttpServer 的工作线程池）持有最后一个引用，
    // 当引用归零时，真正的 `delete` 都会被安全地投递回到该连接原生的 IO 线程执行，
    // 从而绝对避免了「业务线程跨线程 delete -> IO 线程正在读取」的段错误竞态条件。
    std::shared_ptr<TcpConnection> conn(new TcpConnection(ioLoop, fd, ssl), [ioLoop](TcpConnection* p) {
        ioLoop->queueInLoop([p]() {
            delete p;
        });
    });
    conn->setConnId(connId);  // 绑定唯一 ID
    
    // 2. 告诉客人：如果你收到消息，请立刻执行我的 onMessageCallback_！
    conn->setMessageCallback(onMessageCallback_);
    
    // 3. 告诉客人：如果你走了，请调用我的 removeConnection 方法告诉我！
    conn->setCloseCallback(
        [this](const std::shared_ptr<TcpConnection>& c) { removeConnection(c); }
    );
    
    // 4. 把这个新客人登记到账本上（主线程操作 map，安全）
    connections_[connId] = conn;
    LOG_INFO << "TcpServer: 新连接加入账本 connId=" << connId << "，当前连接数=" << connections_.size();

    // 5. 把连接的初始化投递到 IO 线程中执行（包括 enableReading 和心跳监测）
    //    这样可以保证 Channel 注册到正确线程的 epoll 中，且不会在 map 登记前就触发事件
    ioLoop->queueInLoop([conn]() {
        conn->connectEstablished();

    });
}

void TcpServer::removeConnection(const std::shared_ptr<TcpConnection>& conn) {
    // IO 线程触发 close → 必须投递回主线程操作 connections_ map
    loop_->queueInLoop([this, conn]() {
        removeConnectionInLoop(conn);
    });
}

void TcpServer::removeConnectionInLoop(const std::shared_ptr<TcpConnection>& conn) {
    int id = conn->getConnId();
    if (connections_.find(id) != connections_.end()) {
        connections_.erase(id);
        LOG_INFO << "TcpServer: 连接已从账本移除 connId=" << id << "，当前连接数=" << connections_.size();
    }
}

} // namespace MyServer