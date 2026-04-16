/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-20 15:29:51
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-14 19:55:00
 * @FilePath: /ServerPractice/src/TcpConnection.cpp
 * @Description:
 */
#include "TcpConnection.hpp"
#include "EventLoop.hpp"
#include "Channel.hpp"
#include "Logger.hpp"
#include "Buffer.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/errno.h>

namespace MyServer {
    TcpConnection::TcpConnection(EventLoop* loop, int fd, SSL* ssl)
        : loop_(loop), fd_(fd), ssl_(ssl), connId_(-1),
          state_(StateE::kConnecting) {
        channel_ = new Channel(loop_, fd_);
        channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this));
        channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
    }

    TcpConnection::~TcpConnection() {
        /**
         * @brief 安全关闭并释放 SSL 会话对象
         * @details
         *   [SSL_shutdown]
         *   向对端发送 TLS "close_notify" 告警报文，通知对端本侧即将关闭。
         *   这是 TLS 协议规定的有序关闭流程，可防止对端将未发完的数据截断误判为加密异常。
         *
         *   [SSL_free]
         *   释放 SSL 对象持有的所有内部资源（握手状态、会话密钥、BIO 等）。
         *   由于 SSL_set_fd() 内部使用 BIO_NOCLOSE 标志，SSL_free 不会自动 close(fd)，
         *   fd 的关闭由下方 ::close(fd_) 负责。
         */
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        channel_->disableAll();
        delete channel_;
        ::close(fd_);
    }

    void TcpConnection::connectEstablished() {
        channel_->enableReading();
        doTlsHandshake();
    }

    void TcpConnection::handleRead() {
        if (state_ == StateE::kHandshaking) {
            doTlsHandshake();
            return;
        }

        if (state_ != StateE::kConnected) return;
        auto guard = shared_from_this();

        char extrabuf[65536];
        bool gotNewData = false;
        while (true) {
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
            if (readbytes > 0) {
                buffer_.append(extrabuf, readbytes);
                gotNewData = true;
            } else {
                /**
                 * @brief 根据 I/O 操作的返回值提取 OpenSSL 具体的错误码
                 * @signature int SSL_get_error(const SSL *ssl, int ret_code);
                 * @param ssl 当前操作所使用的 SSL 会话对象指针
                 * @param ret_code 最近一次引发错误的 OpenSSL 读/写函数（如 SSL_read/SSL_write）的返回值
                 * @details
                 *   [职责]
                 *   当 SSL_read() / SSL_write() 等 I/O 函数返回 <= 0 的值时，仅表明操作未完成。为了区分是
                 *   底层非阻塞导致的"假错误"，还是由于对端断开、握手异常导致的"真错误"，必须立刻调用本函数来解析。
                 *
                 *   [生命周期与上下文约束]
                 *   重要：调用顺序极为严格！该函数必须紧跟在诱发错误的 I/O 操作之后调用。
                 *   在这两次调用之间，绝不能调用任何其他可能改写全局错误队列的 OpenSSL 函数，
                 *   否则提取到的错误状态可能会受到干扰导致严重 Bug。
                 *
                 *   [各错误码含义及应对策略]
                 *   - SSL_ERROR_WANT_READ:
                 *     底层密文已被榨干，但凑不出一个完整的 TLS 记录用于解密。
                 *     【应对策略】：保持 EPOLLIN 监听，等待新数据到达后重试。
                 *   - SSL_ERROR_WANT_WRITE:
                 *     读操作中 OpenSSL 需要写出底层报文（如 TLS 重协商），但发送缓冲区满。
                 *     【应对策略】：临时注册 EPOLLOUT，等可写后重试。
                 *   - SSL_ERROR_ZERO_RETURN:
                 *     对端发送了 "close_notify"，有序关闭。
                 *   - SSL_ERROR_SYSCALL / SSL_ERROR_SSL:
                 *     不可恢复的底层系统/加密层错误，直接断开。
                 * @return 对应状态分支的完整错误码宏常量（整型）。
                 */
                int sslError = SSL_get_error(ssl_, readbytes);

                if (sslError == SSL_ERROR_WANT_READ) {
                    if (!channel_->isReading()) {
                        channel_->enableReading();
                    }
                    break;
                } else if (sslError == SSL_ERROR_WANT_WRITE) {
                    LOG_ERROR << "SSL_read 引发 WANT_WRITE, 打开 EPOLLOUT";
                    if (!channel_->isWriting()) {
                        channel_->enableWriting();
                    }
                    break;
                } else if (sslError == SSL_ERROR_ZERO_RETURN) {
                    handleClose();
                    break;
                } else {
                    ERR_print_errors_fp(stderr);
                    handleClose();
                    break;
                }
            }
        }
        if (gotNewData && messageCallback_) {
            messageCallback_(guard, &buffer_);
        }
    }

    void TcpConnection::send(const std::string& msg) {
        if (state_ != StateE::kConnected) return;
        if (!writeBuffer_.empty()) {
            writeBuffer_.append(msg);
            return;
        }

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
            int savedErrno = SSL_get_error(ssl_, n);
            if (savedErrno == SSL_ERROR_WANT_WRITE) {
                writeBuffer_.append(msg);
                if (!channel_->isWriting()) {
                    channel_->enableWriting();
                }
            } else if (savedErrno == SSL_ERROR_WANT_READ) {
                writeBuffer_.append(msg);
                if (!channel_->isReading()) {
                    channel_->enableReading();
                }
                if (!channel_->isWriting()) {
                    channel_->enableWriting();
                }
            } else if (savedErrno == SSL_ERROR_ZERO_RETURN) {
                handleClose();
            } else {
                ERR_print_errors_fp(stderr);
                handleClose();
            }
        }
    }

    void TcpConnection::handleWrite() {
        if (state_ == StateE::kHandshaking) {
            doTlsHandshake();
            return;
        }

        if (state_ != StateE::kConnected) return;
        if (writeBuffer_.empty()) {
            channel_->disableWriting();
            // 如果 writeBuffer_ 为空但 EPOLLOUT 被触发，可能是因为
            // SSL_read() 之前返回了 WANT_WRITE（例如 TLS 重协商或 KeyUpdate 期间）。
            // 此时需要在写操作完成后重新尝试读取。
            handleRead();
            return;
        }

        ERR_clear_error();
        ssize_t n = SSL_write(ssl_, writeBuffer_.peek(), writeBuffer_.readableBytes());
        if (n > 0) {
            writeBuffer_.retrieve(n);
        } else {
            int savedErrno = SSL_get_error(ssl_, n);
            if (savedErrno == SSL_ERROR_WANT_WRITE) {
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
                ERR_print_errors_fp(stderr);
                handleClose();
            }
        }

        if (writeBuffer_.empty()) {
            channel_->disableWriting();
        }
    }

    void TcpConnection::forceClose() {
        if (state_ == StateE::kConnected || state_ == StateE::kHandshaking) {
            auto guard = shared_from_this();
            state_ = StateE::kDisconnecting;

            loop_->queueInLoop([this, guard]() {
                channel_->disableAll();
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

        auto guard = shared_from_this();

        if (closeCallback_) {
            closeCallback_(guard);
        }
    }

    void TcpConnection::doTlsHandshake() {
        state_ = StateE::kHandshaking;

        ERR_clear_error();

        /**
         * @brief 发起非阻塞 TLS 握手（自动适配客户端/服务端角色）
         * @signature int SSL_do_handshake(SSL *ssl);
         * @param ssl 已通过 SSL_set_fd() 绑定了底层 socket 的 SSL 会话对象
         * @details
         *   [职责]
         *   SSL_do_handshake() 是 SSL_connect() 和 SSL_accept() 的通用统一版本。
         *   它会根据 SSL 对象的 SSL_CTX 来源自动选择执行何种握手行为：
         *   - 如果 SSL 源自 TLS_client_method() 的 CTX → 执行客户端握手（发送 ClientHello）
         *   - 如果 SSL 源自 TLS_server_method() 的 CTX → 执行服务端握手（等待 ClientHello 并回复）
         *   因此客户端连接（HttpClient 派生）和服务端连接（TcpServer 派生）均可共用此方法。
         * 
         *   [非阻塞握手流程]
         *   在非阻塞 fd 上，SSL_do_handshake() 通常无法一次性完成整个握手流程，
         *   因为中间需要等待对端的响应报文通过网络传输过来。此时它会返回 -1，
         *   通过 SSL_get_error() 可以得到：
         *   - SSL_ERROR_WANT_READ：需要等对端发来数据（注册 EPOLLIN）
         *   - SSL_ERROR_WANT_WRITE：需要等发送缓冲区可写（注册 EPOLLOUT）
         *   后续当 epoll 触发相应事件时，应再次调用 SSL_do_handshake() 继续握手。
         * 
         *   [调用时机]
         *   由 connectEstablished() 在 IO 线程中自动调用，无需外部手动触发。
         * @return 1 表示握手成功；0 表示握手被对端拒绝；-1 表示需要重试或发生错误。
         */
        int ret = SSL_do_handshake(ssl_);

        if (ret == 1) {
            state_ = StateE::kConnected;
            if (connectionCallback_) {
                connectionCallback_(shared_from_this());
            }
            LOG_INFO << "TLS handshake success fd=" << fd_;
        } else {
            int sslError = SSL_get_error(ssl_, ret);

            if (sslError == SSL_ERROR_WANT_READ) {
                if (!channel_->isReading()) {
                    channel_->enableReading();
                }
            } else if (sslError == SSL_ERROR_WANT_WRITE) {
                if (!channel_->isWriting()) {
                    channel_->enableWriting();
                }
            } else {
                ERR_print_errors_fp(stderr);
                LOG_ERROR << "TLS handshake failed fd=" << fd_;
                handleClose();
            }
        }
    }

    void TcpConnection::sendFile(const std::string& filepath) {
        if (state_ != StateE::kConnected) return;

        int fileFd = ::open(filepath.c_str(), O_RDONLY);
        if (fileFd < 0) {
            LOG_ERROR << "Failed to open file: " << filepath;
            return;
        }

        off_t fileSize = ::lseek(fileFd, 0, SEEK_END);
        ::lseek(fileFd, 0, SEEK_SET);

        // SSL 连接无法使用 sendfile() 零拷贝；
        // 必须先读取到用户空间缓冲区，再通过 SSL_write 发送
        char buf[65536];
        off_t remaining = fileSize;
        while (remaining > 0) {
            ssize_t toRead = std::min(static_cast<off_t>(sizeof(buf)), remaining);
            ssize_t nread = ::read(fileFd, buf, toRead);
            if (nread <= 0) break;

            send(std::string(buf, nread));
            remaining -= nread;
        }

        ::close(fileFd);
    }
} // namespace MyServer