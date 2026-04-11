/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-08 15:56:09
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-11 19:19:55
 * @FilePath: /OmniGateway/src/HttpClient.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "HttpClient.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <unistd.h>
#include "SSLManager.hpp"
#include "Logger.hpp"

namespace MyServer {
HttpClient::HttpClient(EventLoop* loop, const InetAddress& serverAddr, const ApiConfig& apiConfig)
    : loop_(loop),
      connector_(std::make_shared<Connector>(loop, serverAddr)),
      apiConfig_(apiConfig),
      connected_(false) {
    
}

void HttpClient::connect() {
    connector_->setNewConnectionCallback([this](int sockfd){
        onConnection(sockfd);
    });
    connector_->start();
}

void HttpClient::onConnection(int sockfd) {
    /**
     * @brief 从 SSL_CTX 工厂中派生一个独立的 SSL 会话对象
     * @signature SSL *SSL_new(SSL_CTX *ctx);
     * @param ctx 全局共享的 SSL 上下文（工厂），这里使用 clientCtx_（客户端模具）
     * @details
     *   [职责]
     *   SSL 对象是单个 TLS 连接的状态容器，它继承了 SSL_CTX 中配置的协议版本、
     *   密码套件、CA 信任链等全局参数，但同时拥有自己独立的握手状态、读写缓冲区和会话密钥。
     * 
     *   [生命周期]
     *   每条 TCP 连接派生一个 SSL 对象。连接关闭时，必须通过 SSL_free() 释放。
     *   SSL 对象的生命周期应当与 TcpConnection 保持一致。
     * 
     * @return 成功返回 SSL 对象指针；失败返回 nullptr（通常表示内存不足）。
     */
    SSL* ssl = SSL_new(SSLManager::getClientCtx());
    if (!ssl) {
        ERR_print_errors_fp(stderr);
        LOG_ERROR << "SSL_new 失败，无法创建 SSL 对象";
        ::close(sockfd);
        return;
    }

    /**
     * @brief 将 SSL 对象绑定到一个已连通的 socket 文件描述符上
     * @signature int SSL_set_fd(SSL *ssl, int fd);
     * @param ssl 需要绑定底层 I/O 通道的 SSL 会话对象
     * @param fd 已完成 TCP 三次握手的非阻塞套接字文件描述符
     * @details
     *   [职责]
     *   该函数内部会自动创建一个 socket 类型的 BIO（Basic I/O Abstraction），
     *   并将其绑定为 SSL 对象的读写 BIO。这样后续调用 SSL_read / SSL_write 时，
     *   OpenSSL 就知道从哪个 fd 收发底层密文数据。
     * 
     *   [为什么不需要手动创建 BIO？]
     *   BIO 是 OpenSSL 的 I/O 抽象层，分为 source/sink BIO（如 socket、memory）
     *   和 filter BIO（如 buffering、base64）。SSL_set_fd() 是一个便捷快捷方式，
     *   等价于手动执行：
     *     BIO* bio = BIO_new_socket(fd, BIO_NOCLOSE);
     *     SSL_set_bio(ssl, bio, bio);
     *   对于直接操作 socket fd 的场景，SSL_set_fd() 完全足够，
     *   无需手动接触 BIO 层。
     * 
     *   [BIO_NOCLOSE 语义]
     *   SSL_set_fd 内部创建的 BIO 使用 BIO_NOCLOSE 标志，表示当 SSL_free()
     *   销毁 SSL 对象时，不会自动 close(fd)。fd 的关闭职责留给 TcpConnection 的析构函数。
     * 
     * @return 成功返回 1；失败返回 0。
     */
    if (!SSL_set_fd(ssl, sockfd)) {
        ERR_print_errors_fp(stderr);
        LOG_ERROR << "SSL_set_fd 失败，无法绑定 fd=" << sockfd;
        SSL_free(ssl);
        ::close(sockfd);
        return;
    }

    // 创建 TcpConnection，将 SSL 对象传入，后续读写将自动走加密通道
    // 参照 TcpServer::newConnection 的模式，使用自定义删除器确保 delete 在 IO 线程执行
    std::shared_ptr<TcpConnection> conn(new TcpConnection(loop_, sockfd, ssl), [this](TcpConnection* p) {
        loop_->queueInLoop([p]() {
            delete p;
        });
    });

    conn->setMessageCallback(
        [this] (const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
            onMessage(conn, buf);
        }
    );
    conn->setCloseCallback(
        [this] (const std::shared_ptr<TcpConnection>& conn) {
            onClose(conn);
        }
    );

    backendConn_ = conn;
    connected_ = true;

    // 在 IO 线程中安全地注册 epoll 读事件，然后发起 TLS 握手
    loop_->queueInLoop([conn]() {
        conn->connectEstablished();
        conn->doTlsHandshake();
    });
}

void HttpClient::onClose(const std::shared_ptr<TcpConnection>& conn) {
    
}

void HttpClient::onMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
    
}

void HttpClient::sendRequest(const std::string& request) {
    
}
}// namespace MyServer