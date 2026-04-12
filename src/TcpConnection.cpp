/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-20 15:29:51
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-12 15:18:11
 * @FilePath: /ServerPractice/src/TcpConnection.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "TcpConnection.hpp"
#include "EventLoop.hpp"
#include "Channel.hpp"
#include "Logger.hpp"
#include "Buffer.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <sys/sendfile.h> // 提供 sendfile 函数
#include <fcntl.h> // 提供 open 函数和 O_RDONLY 标志
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/errno.h>

namespace MyServer {
    TcpConnection::TcpConnection(EventLoop* loop, int fd, SSL* ssl)
        : loop_(loop), fd_(fd), ssl_(ssl), connId_(-1),
          state_(ssl ? StateE::kConnecting : StateE::kConnected) {
        channel_ = new Channel(loop_, fd_);
        // 绑定读事件的回调函数
        channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this));
        channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
        // 注意：不再在构造函数中 enableReading，必须等到 IO 线程中通过 connectEstablished() 来完成注册
    }

    TcpConnection::~TcpConnection() {
        channel_->disableAll();
        delete channel_;
        ::close(fd_); // 必须关闭底层文件描述符，防止幽灵连接
    }

    void TcpConnection::connectEstablished() {
        channel_->enableReading(); // 在 IO 线程中安全地注册 epoll 读事件
    }

    void TcpConnection::handleRead() {
        if (state_ == StateE::kHandshaking) {
            doTlsHandshake();
            return;
        }

        int savedErrno = 0;
        // kHandshaking 时也需要进入（TLS 握手可能需要读取对端报文）
        if (state_ != StateE::kConnected) return;
        // 使用智能指针守卫，防止在回调过程中自己被析构导致崩溃
        auto guard = shared_from_this();

        if (ssl_) { // 密文数据
            char extrabuf[65536]; // 栈上缓冲区，用于接收 SSL 解密后的数据
            while (true) { // 边缘触发模式下，必须使用 while 循环榨干数据
                /**
                 * @brief 清空当前线程的 OpenSSL 错误队列
                 * @signature void ERR_clear_error(void);
                 * @details 
                 *   [职责]
                 *   在调用可能会产生新错误的 OpenSSL API（如 SSL_read 或 SSL_write）之前，
                 *   必须清理错误队列，避免将之前遗留的（可能是其他操作或忽略的）错误
                 *   误认为是当前操作产生的错误。
                 * 
                 *   [工作流程]
                 *   1. 访问当前线程局部的 OpenSSL 错误记录堆栈。
                 *   2. 释放并清空所有挂起的旧错误记录（包括错误码、文件、行号等上下文）。
                 *   3. 重置状态，为后续真实的错误捕获提供干净的画布。
                 * 
                 *   [生命周期与上下文]
                 *   OpenSSL 的错误队列是线程局部存储（Thread-Local Storage, TLS）的。
                 *   这意味着虽然多个并发连接可能在同一个或不同的 IO 线程被处理，
                 *   但此函数只安全地清空当前执行线程的队列，完全契合多线程 Reactor 模型。
                 * 
                 * @return 无返回值。
                 */
                ERR_clear_error();
                
                int readbytes = SSL_read(ssl_, extrabuf, sizeof(extrabuf));
                if (readbytes > 0) { // 成功读取到数据
                    buffer_.append(extrabuf, readbytes);
                    extendLife();
                } else { 
                    /**
                     * @brief 根据 I/O 操作的返回值提取 OpenSSL 具体的错误码
                     * @signature int SSL_get_error(const SSL *ssl, int ret_code);
                     * @param ssl 当前操作所使用的 SSL 会话对象指针
                     * @param ret_code 最近一次引发错误的 OpenSSL 读/写函数（如 SSL_read/SSL_write）的返回值
                     * 
                     * @details 
                     *   [职责]
                     *   当 SSL_read() / SSL_write() 等 I/O 函数返回 <= 0 的值时，仅表明操作未完成。为了区分是
                     *   底层非阻塞导致的“假错误”，还是由于对端断开、握手异常导致的“真错误”，必须立刻调用本函数来解析。
                     * 
                     *   [生命周期与上下文约束]
                     *   重要：调用顺序极为严格！该函数必须紧跟在诱发错误的 I/O 操作之后调用。
                     *   在这两次调用之间，绝不能调用任何其他可能改写全局错误队列的 OpenSSL 函数，
                     *   否则提取到的错误状态可能会受到干扰导致严重 Bug。
                     * 
                     *   [各个错误信息表示的含义及生命周期应对策略]
                     *   - SSL_ERROR_NONE:
                     *     说明底层的 I/O 操作实际上是成功的（由于本分支是针对 readbytes <= 0 的拦截，通常不会是这个码）。
                     * 
                     *   - SSL_ERROR_WANT_READ:
                     *     说明当前连接正在等待对端发送更多数据。例如：在读取应用数据时，底层收取到了部分密文，
                     *     但其长度不足以被 AES 层解密拼凑出一个完整的 TLS 记录（TLS Record），导致操作处于“受阻渴望数据”状态。
                     *     【应对策略】：在 epoll 中保持或激活对此文件描述符的 EPOLLIN (读) 监听，一旦新数据抵达底座，即可重试。
                     * 
                     *   - SSL_ERROR_WANT_WRITE:
                     *     说明当前连接需要向对端发送数据，但本端的底层系统发送缓冲区已被塞满。为什么读操作(SSL_read)
                     *     也会引发写受阻？因为 TLS 协议存在 "Renegotiation(重协商)" 会话机制，此时要求引擎偷偷地双向通信。
                     *     【应对策略】：在 epoll 中临时添加对此文件描述符的 EPOLLOUT (写) 监听，等底座变成可写状态时重试。
                     * 
                     *   - SSL_ERROR_ZERO_RETURN:
                     *     说明对端的 TLS 栈发送了纯正的 "close notify" 控制报文，宣告其完成了数据的发送任务，期望有序告别。
                     *     【应对策略】：我们收到此信号后，也应妥善走完应用层的清理工作，关闭套接字并断开自身。
                     * 
                     *   - SSL_ERROR_SYSCALL:
                     *     遇到了惨烈的、不可恢复的底层 Linux 操作系统连接溃败（如遭到 RST，或断网导致的 Pipe Broken）。
                     *     【应对策略】：需使用传统的 errno 来获取系统调用失败源头，之后彻底废弃此连接。
                     * 
                     *   - SSL_ERROR_SSL:
                     *     遇到了致命的、不可重试的加密层异常。典型的重金灾区：MAC (消息摘要) 计算未过关被发现遭遇中间人纂改、
                     *     客户端私自发来不受支持的降级协议等。
                     *     【应对策略】：此时可以通过前人留下的 ERR_print_errors 打印具体错误原因链路，然后毫不犹豫地拔除该连接。
                     * 
                     * @return 转换后能够直观表示上述某种状态分支的专属宏常量（整型）。
                     */
                    int sslError = SSL_get_error(ssl_, readbytes);
                    
                    if (sslError == SSL_ERROR_WANT_READ) {
                        // 情况一：底层密文已经被榨干，但是凑不出一个完整的解密块
                        // 保持此时的 EPOLLIN 监听状态，退出循环
                        if (!channel_->isReading()) {
                            channel_->enableReading();
                        }
                        break;
                    } else if (sslError == SSL_ERROR_WANT_WRITE) {
                        // 情况三：读着读着，OPEN_SSL 突然需要发送底层报文，且发送不出去
                        // 保持此时的 EPOLLOUT 监听状态，退出循环
                        LOG_ERROR << "SSL_read 引发 WANT_WRITE, 打开 EPOOLOUT";
                        if (!channel_->isWriting()) {
                            channel_->enableWriting();
                        }
                        break;
                    } else if (sslError == SSL_ERROR_ZERO_RETURN) {
                        // 对端发送了 TLS "Close Notify" 告警报文，正在有序关闭 SSL 连接
                        handleClose();
                        break;
                    } else {
                        // 出现不可恢复的硬性错误，如（解密失败，证书错误，物理网络断开）
                        ERR_print_errors_fp(stderr);
                        handleClose();
                        break;
                    }
                }
            }
        } else { // 明文数据
            /**
            * @brief 通过 Buffer::readFd() 从套接字高效读取数据
            * @details readFd 内部使用 readv() scatter/gather IO + ET 循环榨干策略：
            *   - iov[0] 指向 Buffer 自身的可写区域（堆内存）
            *   - iov[1] 指向栈上 64KB 的 extrabuf（溢出着陆区）
            *   - 在 while(true) 中循环调用 readv，直到 EAGAIN 表示内核缓冲区已抽干
            * @return >0 实际读取总字节数；0 对端关闭；-1 不可恢复错误
            */
            ssize_t n = buffer_.readFd(fd_, &savedErrno);

            if (n > 0) {
                // 成功读取到数据，续命并触发业务层回调
                extendLife();
                if (messageCallback_) {
                    messageCallback_(guard, &buffer_);
                }
            } else if (n == 0) {
                // 对端关闭连接（收到 FIN）
                LOG_INFO << "客户端 fd " << fd_ << " 断开连接";
                state_ = StateE::kDisconnecting;
                channel_->disableAll(); // 立即从 epoll 中注销，防止后续事件重入
                auto cb = closeCallback_;
                if (cb) {
                    cb(guard);
                }
            } else {
                // 不可恢复的读取错误
                LOG_ERROR << "readFd 失败! errno=" << savedErrno;
                state_ = StateE::kDisconnecting;
                channel_->disableAll();
                auto cb = closeCallback_;
                if (cb) {
                    cb(guard);
                }
            }
        }
    }

    void TcpConnection::send(const std::string& msg) {
        if (state_ != StateE::kConnected) return;
        int savedErrno = 0;
        if (!writeBuffer_.empty()) {
            writeBuffer_.append(msg);
            return;
        }

        // 1. 如果是 SSL 连接，需要加密
        if (ssl_) {
            ERR_clear_error();
            ssize_t n = SSL_write(ssl_, msg.c_str(), msg.size());
            if (n > 0) {
                if (static_cast<size_t>(n) < msg.size()) {
                    writeBuffer_.append(msg.c_str() + n, msg.size() - n);
                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                }
            } else {
                savedErrno = SSL_get_error(ssl_, n);
                if (savedErrno == SSL_ERROR_WANT_WRITE) {
                    // 情况二：尝试发送加密数据，但是底层缓冲区满了
                    writeBuffer_.append(msg);
                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                } else if (savedErrno == SSL_ERROR_WANT_READ) {
                    // 情况四：应用层想要发送数据，但是 OPEN_SSL 发现 TLS 握手还未完成
                    // 开启可读监听，等待对方发送 TLS 握手数据
                    writeBuffer_.append(msg);
                    if (!channel_->isReading()) {
                        channel_->enableReading();
                    }
                    // 重协商完后，需要重新发送数据
                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                } else if (savedErrno == SSL_ERROR_ZERO_RETURN) {
                    // 对端发送了 TLS "Close Notify" 告警报文，正在有序关闭 SSL 连接
                    handleClose();
                } else {
                    // 出现不可恢复的硬性错误，如（解密失败，证书错误，物理网络断开）
                    ERR_print_errors_fp(stderr);
                    handleClose();
                }
            }
        } else {
            // 输出缓冲区为空，直接发送
            ssize_t n = ::write(fd_, msg.c_str(), msg.size());
            bool faltError = false;

            if (n > 0) {
                if (static_cast<size_t>(n) < msg.size()) {
                    // 发送了一部分，把剩下的追加到输出缓冲区
                    writeBuffer_.append(msg.c_str() + n, msg.size() - n);
                    
                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                }
            } else {
                savedErrno = errno;
                if (savedErrno == EWOULDBLOCK || savedErrno == EAGAIN) {
                    // 缓存区满了，开启写事件监听
                    writeBuffer_.append(msg);
                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                } else {
                    LOG_ERROR << "write 失败! errno=" << savedErrno;
                    faltError = true;
                }
            }

            if (faltError) {
                handleClose();
            }
        }
    }

    void TcpConnection::handleWrite() {
        if (state_ == StateE::kHandshaking) {
            doTlsHandshake();
            return;
        }

        // kHandshaking 时也需要进入（TLS 握手可能需要写入 ClientHello 等报文）
        if (state_ != StateE::kConnected) return;
        int savedErrno = 0;
        ssize_t n = 0;
        if (writeBuffer_.empty()) {
            channel_->disableWriting();
            return;
        }

        if (ssl_) {
            ERR_clear_error();
            n = SSL_write(ssl_, writeBuffer_.peek(), writeBuffer_.readableBytes());
            if (n > 0) {
                writeBuffer_.retrieve(n);
            } else {
                savedErrno = SSL_get_error(ssl_, n);
                if (savedErrno == SSL_ERROR_WANT_WRITE) {
                    // 缓存区满了，开启写事件监听
                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                } else if (savedErrno == SSL_ERROR_WANT_READ) {
                    if (!channel_->isReading()) {
                        channel_->enableReading();
                    }

                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                } else {
                    // 出现不可恢复的硬性错误，如（解密失败，证书错误，物理网络断开）
                    ERR_print_errors_fp(stderr);
                    handleClose();
                }
            }
        } else {
            n = ::write(fd_, writeBuffer_.peek(), writeBuffer_.readableBytes());
            if (n > 0) {
                writeBuffer_.retrieve(n);
            } else {
                savedErrno = errno;
                
                if (savedErrno == EWOULDBLOCK || savedErrno == EAGAIN) {
                    // 缓存区满了，开启写事件监听
                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                } else {
                    LOG_ERROR << "write 失败! errno=" << savedErrno;
                    handleClose();
                }
            }
        }

        if (writeBuffer_.empty()) {
            channel_->disableWriting();
        }
    }

    void TcpConnection::extendLife() {
        // 1. 如果之前已经有一个秒表了，我们直接把它“标记删除”（惰性删除，O(1)复杂度）
        // 这样大管家在处理时会自动忽略它，极其高效！
        if (keepAliveTimer_) {
            keepAliveTimer_->setDeleted();
        }

        // 2. 重新开启一个 30 秒的定时器！
        std::weak_ptr<TcpConnection> weak_conn = shared_from_this();

        keepAliveTimer_ = loop_->runAfter(30000, [weak_conn]() {
            // 闹钟响了，尝试把 weak_ptr 提升为 shared_ptr
            auto conn = weak_conn.lock();
            if (conn) {
                // 如果提升成功，说明连接还没被常规途径关闭，立刻执行踢人逻辑！
                conn->handleTimeout();
            }
        });
    }

    void TcpConnection::handleTimeout() {
        LOG_WARNING << "客户端 fd " << fd_ << " 长时间未发送数据，心跳超时，强制踢出！";
        // kHandshaking 或 kConnected 均可被超时踢出
        if (state_ == StateE::kDisconnecting || state_ == StateE::kDisconnected) return;
        state_ = StateE::kDisconnecting;
        channel_->disableAll(); // 立即从 epoll 中注销，防止后续事件重入
        // 触发关闭回调，TcpServer 会负责把它从账本里删掉，并销毁堆内存
        auto guard = shared_from_this();
        if (closeCallback_) {
            closeCallback_(guard);
        }
    }

    void TcpConnection::forceClose() {
        if (state_ == StateE::kConnected || state_ == StateE::kHandshaking) {
            auto guard = shared_from_this();
            // 1. 立即修改状态机，防止新的读写事件被调度
            state_ = StateE::kDisconnecting;
            // 2. 取消所有定时器
            if (keepAliveTimer_) {
                keepAliveTimer_->setDeleted();
            }
            // 将任务丢回到 I/O 线程中处理
            loop_->queueInLoop([this, guard]() {
                channel_->disableAll(); // 在 IO 线程中注销 epoll，防止后续事件重入
                if (closeCallback_) {
                    closeCallback_(guard);
                }
            });
        }
    }

    void TcpConnection::handleClose() {
        if (state_ == StateE::kDisconnecting || state_ == StateE::kDisconnected) return;

        state_ = StateE::kDisconnected;
        channel_->disableAll();

        if (keepAliveTimer_) {
            keepAliveTimer_->setDeleted();
        }
        
        auto guard = shared_from_this();
        
        if (closeCallback_) {
            closeCallback_(guard);
        }
    }

    void TcpConnection::doTlsHandshake() {
        if (!ssl_) return; // 明文连接不需要握手

        state_ = StateE::kHandshaking;

        ERR_clear_error();

        /**
         * @brief 发起客户端侧的 TLS 握手
         * @signature int SSL_connect(SSL *ssl);
         * @param ssl 已通过 SSL_set_fd() 绑定了底层 socket 的 SSL 会话对象
         * @details
         *   [职责]
         *   SSL_connect() 是 SSL_do_handshake() 的客户端特化版本。
         *   它执行完整的 TLS 握手流程：
         *   - 发送 ClientHello → 接收 ServerHello + 证书 → 密钥交换 → 发送 Finished
         * 
         *   [非阻塞行为]
         *   在非阻塞 fd 上，SSL_connect() 通常无法一次性完成整个握手流程，
         *   因为中间需要等待对端的响应报文通过网络传输过来。此时它会返回 -1，
         *   通过 SSL_get_error() 可以得到：
         *   - SSL_ERROR_WANT_READ：需要等对端发来数据（注册 EPOLLIN）
         *   - SSL_ERROR_WANT_WRITE：需要等发送缓冲区可写（注册 EPOLLOUT）
         *   后续当 epoll 触发相应事件时，应再次调用 SSL_connect() 继续握手。
         * 
         * @return 1 表示握手成功；0 表示握手被对端拒绝；-1 表示需要重试或发生错误。
         */
        int ret = SSL_connect(ssl_);

        if (ret == 1) {
            // 握手成功！切换到正常连接状态
            state_ = StateE::kConnected;
            if (connectionCallback_) {
                connectionCallback_(shared_from_this());
            }
            LOG_INFO << "TLS 握手成功 fd=" << fd_;
        } else {
            int sslError = SSL_get_error(ssl_, ret);

            if (sslError == SSL_ERROR_WANT_READ) {
                // 需要等待对端发送握手数据，确保 EPOLLIN 已注册
                if (!channel_->isReading()) {
                    channel_->enableReading();
                }
            } else if (sslError == SSL_ERROR_WANT_WRITE) {
                // 需要等待发送缓冲区可写，注册 EPOLLOUT
                if (!channel_->isWriting()) {
                    channel_->enableWriting();
                }
            } else {
                // 不可恢复的握手错误
                ERR_print_errors_fp(stderr);
                LOG_ERROR << "TLS 握手失败 fd=" << fd_;
                handleClose();
            }
        }
    }
}