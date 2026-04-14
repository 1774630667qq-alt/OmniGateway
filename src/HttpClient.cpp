/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-08 15:56:09
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-14 10:35:04
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
#include "ThreadPool.hpp"
#include "HttpParser.hpp"

namespace MyServer {
HttpClient::HttpClient(EventLoop* loop, ThreadPool* pool, const std::string& hostname, int port, ApiConfig& apiConfig)
    : loop_(loop),
    threadPool_(pool),
    targethost_(hostname),
    targetport_(port),
    apiConfig_(apiConfig),
    connected_(false) {
    
}

HttpClient::~HttpClient() {
    if (connected_ && backendConn_) {
        backendConn_->forceClose();
        backendConn_.reset();
    }
}

void HttpClient::connect() {
    LOG_INFO << "开始异步解析域名"<< targethost_ << ":" << targetport_;
    
    auto self = shared_from_this();

    threadPool_->enqueue([this, self]() {
        std::string resolvedIp;
        
        bool success = InetAddress::resolve(targethost_, resolvedIp);
        
        loop_->queueInLoop([this, self, success, resolvedIp]() {
            if (success) {
                LOG_INFO << "域名解析成功: " << targethost_ << " -> " << resolvedIp;
                
                InetAddress addr(targetport_, resolvedIp);
                
                self->doConnection(addr);
            } else {
                LOG_ERROR << "域名解析失败: " << targethost_;
                
                if (responseCallback_) {
                    responseCallback_("data: {\"error\": \"DNS Resolution Failed\"}\n\n");
                    responseCallback_("data: [DONE]\n\n");
                }
            }
        });
    });
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
     * @brief 显式将 SSL 对象设置为客户端模式（主动发起握手方）
     * @signature void SSL_set_connect_state(SSL *ssl);
     * @param ssl 目标 SSL 会话对象
     * @details
     *   [职责]
     *   标记该 SSL 对象在后续握手中扮演"客户端"角色（即发送 ClientHello 的一方）。
     *   与之对应的 SSL_set_accept_state() 则将 SSL 对象设置为服务端模式。
     * 
     *   [在本项目中是否必要？]
     *   严格来说是冗余的，原因：
     *   1. SSL 对象由 TLS_client_method() 创建的 clientCtx_ 派生，默认已是客户端模式。
     *   2. 后续调用的 SSL_connect() 内部也会隐式调用 SSL_set_connect_state()。
     *   此处显式调用作为防御性编程，明确表达"这是一条客户端连接"的设计意图。
     */
    SSL_set_connect_state(ssl);

    // 【关键修复】：设置 SNI (Server Name Indication)
    // 如果不设置，像 Edge、Cloudflare 或大多数 API 网关会在握手时直接挂起或返回 403/404
    SSL_set_tlsext_host_name(ssl, apiConfig_.host.c_str());

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
    // 注册连接回调：TLS 握手成功后自动将暂存的请求发出
    conn->setConnectionCallback(
        [this] (const std::shared_ptr<TcpConnection>&) {
            sendRequest();
        }
    );

    backendConn_ = conn;
    connected_ = true;

    // 在 IO 线程中安全地注册 epoll 读事件并自动发起 TLS 握手
    loop_->queueInLoop([conn]() {
        conn->connectEstablished();
    });
}

/**
 * @brief 后端连接物理关闭时的回调
 * @details 释放 backendConn_ 引用，并通过 responseCallback_ 发送异常熔断信号，
 *   防止前端在网络突然中断时无限等待。
 */
void HttpClient::onClose(const std::shared_ptr<TcpConnection>& conn) {
    LOG_INFO << "HttpClient::onClose 触发: 与后端 API (" 
             << apiConfig_.host << ") 的连接已物理断开";
    
    connected_ = false;

    // 1. 释放对 TcpConnection 的强引用，允许其安全析构
    if (backendConn_ && backendConn_ == conn) {
        backendConn_.reset();
    }

    // 【关键修复】：不能直接 responseCallback_ = nullptr！
    // 如果 lambda 捕获了 shared_ptr<HttpClient>（client），直接赋 nullptr 会触发：
    //   ~lambda → ~client → ~HttpClient → ~responseCallback_（二次析构）→ 崩溃
    // 正确做法：move 到局部变量，让回调在 onClose 返回后才析构。
    // 同时在析构前发送熔断信号。
    auto oldCallback = std::move(responseCallback_);
    // responseCallback_ 现在是空的（moved-from），不会被 ~HttpClient 再次析构

    // 2. 异常熔断通知（防死锁兜底机制）
    // 如果大模型正常发完，最后一条通常是 data: [DONE]\n\n，前端收到就会自动结束。
    // 但如果网络突然断了，我们最好通过回调给前端发一个伪造的异常断开信号，防止前端死等。
    if (oldCallback) {
        std::string errorSignal = "data: {\"error\": \"Gateway backend connection closed unexpectedly\"}\n\n";
        oldCallback(errorSignal);
    }
    // oldCallback 在这里析构，此时 onClose 即将返回，安全。
}

/**
 * @brief 后端 API 返回数据时的核心解析入口
 * @details 采用三阶段流水线处理：
 *   阶段一：委托 httpParser_ 解析响应状态行和头部
 *   阶段二：若状态码非 200，收集完整错误实体后回传
 *   阶段三：Chunked 脱壳 → SSE 事件切割 → 逐条回传前端
 */
void HttpClient::onMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
    
    // ================= 阶段一：解析 HTTP 头部 =================
    if (!headersParsed_) {
        bool ok = httpParser_.parse(buf);
        if (!ok) {
            LOG_ERROR << "HTTP 响应头解析失败";
            conn->forceClose();
            return;
        }

        // 【关键防御】：如果头部还没收到 \r\n\r\n，立刻交出控制权，杜绝死等
        if (!httpParser_.gotAll()) {
            return; 
        }

        // 头部彻底解析完成，开始提取核心参数
        headersParsed_ = true;
        int code = httpParser_.statusCode();

        if (code == 200) {
            if (httpParser_.getHeader("Transfer-Encoding") == "chunked") {
                parseState_ = ParseState::kExpectChunkSize; // 跃迁至脱壳阶段
            } else {
                LOG_ERROR << "未收到 chunked 编码，不支持处理该流";
                conn->forceClose();
                return;
            }
        } else {
            // 非 200 状态码，跃迁至接收错误实体阶段
            parseState_ = ParseState::kExpectErrorBody;
            std::string context_len = httpParser_.getHeader("Content-Length");
            if (!context_len.empty()) {
                // 使用 stoull 防止大文件引发的溢出异常
                expectedErrorLength_ = std::stoull(context_len);
            } else {
                LOG_WARNING << "非 200 响应缺失 Content-Length，强制断开";
                conn->forceClose();
                return;
            }
        }
    }

    // ================= 阶段二：处理异常状态码的完整实体 =================
    if (parseState_ == ParseState::kExpectErrorBody) {
        // 【核心机制】：如果积压的数据够了，才提取并断开
        if (buf->readableBytes() >= expectedErrorLength_) {
            std::string errorBody = buf->retrieveAsString(expectedErrorLength_);
            if (responseCallback_) {
                responseCallback_(errorBody);
            }
            LOG_ERROR << "HttpClient 接收到报错:\n" << errorBody;
            conn->forceClose(); // 错误收集完毕，物理断开连接
        }
        return; 
    }

    // ================= 阶段三：处理 Chunked 和 SSE 报文 =================
    while (parseState_ == ParseState::kExpectChunkSize || parseState_ == ParseState::kExpectChunkData) {
        if (parseState_ == ParseState::kExpectChunkSize) {
            size_t crlfPos = buf->findCRLF(std::string_view("\r\n", 2));
            if (crlfPos == std::string::npos) {
                return;
            }
            std::string hexStr(buf->peek(), crlfPos);
            try {
                currentChunkSize_ = std::stoull(hexStr, nullptr, 16);
            } catch (const std::exception& e) {
                LOG_ERROR << "HttpClient::onMessage: 解析 chunk 大小失败: " << e.what();
                conn->forceClose();
                return;
            }
            buf->retrieve(crlfPos + 2);

            if (currentChunkSize_ == 0) {
                LOG_INFO << "大模型API响应结束, 物理断开连接";
                conn->forceClose();
                return;
            }

            parseState_ = ParseState::kExpectChunkData;
        }

        if (parseState_ == ParseState::kExpectChunkData) {
            if (buf->readableBytes() < currentChunkSize_ + 2) {
                // 半包防御，等待更多数据
                return;
            }
            
            chunkedBuffer_.append(buf->peek(), currentChunkSize_);
            buf->retrieve(currentChunkSize_ + 2);
            currentChunkSize_ = 0;
            parseState_ = ParseState::kExpectChunkSize;

            // 对脱壳后的数据进行 SSE 事件切割
            while (true) {
                size_t crlfPos = chunkedBuffer_.findCRLF(std::string_view("\n\n", 2));
                if (crlfPos == std::string::npos) { // 没有接收到完整数据重新等待
                    break;
                }
                std::string sseData(chunkedBuffer_.peek(), crlfPos);
                chunkedBuffer_.retrieve(crlfPos + 2);
                if (responseCallback_) {
                    responseCallback_(sseData);
                }
            }
        }
    }
}

/**
 * @brief 组装并发送完整的 HTTP POST 请求
 * @details 将 pendingRequestBody_ 包装为合法 HTTP 报文（请求行 + 请求头 + 空行 + 请求体），
 *   通过 backendConn_->send() 发出。由 connectionCallback_ 在 TLS 握手成功后自动调用。
 */
void HttpClient::sendRequest() {
    if (!connected_ || !backendConn_) {
        LOG_ERROR << "HttpClient::sendRequest: 未连接到后端 API";
        return;
    }

    if (pendingRequestBody_.empty()) {
        LOG_WARNING << "HttpClient::sendRequest: 没有待发送的请求体";
        return;
    }

    std::string httpPacket;
    // 拼接请求行
    httpPacket += "POST " + apiConfig_.path + " HTTP/1.1\r\n";
    // 拼接请求头
    httpPacket += "Host: " + apiConfig_.host + "\r\n";
    httpPacket += "Content-Type: application/json\r\n";
    httpPacket += "Authorization: Bearer " + apiConfig_.apiKey + "\r\n";
    httpPacket += "Content-Length: " + std::to_string(pendingRequestBody_.size()) + "\r\n";
    httpPacket += "\r\n";
    // 拼接请求体
    httpPacket += pendingRequestBody_;
    // 发送 HTTP 请求
    backendConn_->send(httpPacket);

    LOG_INFO << "HTTP 请求已发送至后端 API: " << apiConfig_.host << apiConfig_.path;
}

void HttpClient::doConnection(const InetAddress& addr) {
    connector_ = std::make_shared<Connector>(loop_, addr);
    connector_->setNewConnectionCallback([this] (int sockfd) {
        this->onConnection(sockfd);
    });

    connector_->start();
}
}// namespace MyServer