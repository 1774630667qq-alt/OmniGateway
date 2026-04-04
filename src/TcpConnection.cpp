/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-20 15:29:51
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-31 14:03:32
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

namespace MyServer {
    TcpConnection::TcpConnection(EventLoop* loop, int fd)
        : loop_(loop), fd_(fd), connId_(-1), state_(StateE::kConnected) {
        channel_ = new Channel(loop_, fd_);
        // 绑定读事件的回调函数
        channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this));
        channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
        // 注意：不再在构造函数中 enableReading，必须等到 IO 线程中通过 connectEstablished() 来完成注册
    }

    TcpConnection::~TcpConnection() {
        // 析构时必须关闭底层文件描述符，防止幽灵连接
        while (!outputQueue_.empty()) {
            OutputItem& item = outputQueue_.front();
            if (item.is_file) {
                ::close(item.file_fd);
            }
            outputQueue_.pop();
        }
        channel_->disableAll();
        delete channel_;
        ::close(fd_); // 必须关闭底层文件描述符，防止幽灵连接
    }

    void TcpConnection::connectEstablished() {
        channel_->enableReading(); // 在 IO 线程中安全地注册 epoll 读事件
    }

    void TcpConnection::handleRead() {
        // 如果连接已经不在正常状态，直接忽略，防止重入
        if (state_ != StateE::kConnected) return;
        // 使用智能指针守卫，防止在回调过程中自己被析构导致崩溃
        auto guard = shared_from_this();
        int active_fd = channel_->getFd();
        while (true) {
            char buf[1024];
            /**
             * @brief 从套接字接收数据 (系统调用)
             * @param sockfd  用于接收数据的套接字文件描述符 (此处的 active_fd)
             * @param buf     指向存放接收数据缓冲区的指针
             * @param len     缓冲区的最大长度
             * @param flags   接收操作的标志位，通常设为 0
             * @return 成功返回实际接收到的字节数；返回 0 表示对端已正常关闭连接；失败返回 -1 并设置 errno
             */
            int bytes_read = recv(active_fd, buf, sizeof(buf), 0);
            if (bytes_read > 0) {
                buffer_.append(buf, bytes_read); // 把读到的数据追加到缓冲区
                // 每次读到数据都要续命一下，重置秒表
                extendLife();
            } else if (bytes_read == -1){
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 没有更多数据可读了，退出循环
                    break;
                } else {
                    LOG_ERROR << "Recv 失败!";
                    state_ = StateE::kDisconnecting;
                    channel_->disableAll(); // 立即从 epoll 中注销，防止后续事件重入
                    // 将回调拷贝到栈上，防止对象自杀导致段错误
                    auto cb = closeCallback_;
                    if (cb) {
                        cb(guard);
                    }
                    return; // close 后直接返回，不再执行 messageCallback_
                }
            } else {
                // bytes_read == 0，客户端断开连接
                LOG_INFO << "客户端 fd " << active_fd << " 断开连接";
                state_ = StateE::kDisconnecting;
                channel_->disableAll(); // 立即从 epoll 中注销，防止后续事件重入
                // 同理，拷贝到栈上安全执行
                auto cb = closeCallback_;
                if (cb) {
                    cb(guard);
                }
                return; // close 后直接返回，不再执行 messageCallback_
            }
        }

        if (state_ == StateE::kConnected) {
            if (messageCallback_) {
                messageCallback_(guard, &buffer_);
            }
        }
    }

    void TcpConnection::send(const std::string& msg) {
        if (state_ != StateE::kConnected) return; // 连接已断开，不再发送
        // 不论如何先把消息放到发送队列里，保证发送的顺序
        outputQueue_.push(OutputItem(msg));
        if (outputQueue_.size() == 1) { // 如果之前队列是空的，说明 handleWrite() 没有在工作，需要踢一脚它了
            handleWrite(); // 直接调用一次 handleWrite()，尝试把数据发送出去，后续如果没发完会自动注册 EPOLLOUT 继续发送
        }
    }

    void TcpConnection::sendFile(const std::string& filepath) {
        if (state_ != StateE::kConnected) return; // 连接已断开，不再发送
        /**
         * @brief 打开一个文件并获取其文件描述符 (系统调用)
         * @signature int open(const char *pathname, int flags);
         * @param pathname 指向要打开的文件路径的 C 风格字符串
         * @param flags    文件访问模式和状态标志位：
         *                 - O_RDONLY: 以只读模式打开文件。(因为发文件只需读取磁盘内容)
         *                 - O_WRONLY: 以只写模式打开文件。
         *                 - O_RDWR:   以读写模式打开文件。
         *                 (还可使用按位或组合其他标志，例如 O_CREAT 创建文件、O_NONBLOCK 非阻塞打开等)
         * @return 成功返回一个新的非负文件描述符；失败返回 -1 并自动设置 errno (例如文件不存在或权限不足)
         */
        int file_fd = ::open(filepath.c_str(), O_RDONLY);
        if (file_fd == -1) {
            LOG_ERROR << "打开文件失败: " << filepath;
            return;
        }
        /**
         * @brief 重新定位文件描述符的读写偏移量 (系统调用)
         * @signature off_t lseek(int fd, off_t offset, int whence);
         * @param fd      由 open() 返回的文件描述符
         * @param offset  相对于 whence 的偏移量，可以是正数或负数
         * @param whence  偏移量的基准位置：
         *                - SEEK_SET: 从文件开头计算偏移。
         *                - SEEK_CUR: 从当前文件指针位置计算偏移。
         *                - SEEK_END: 从文件末尾计算偏移。
         * @return 成功返回从文件开头到新位置的偏移量（字节数）；失败返回 -1 并设置 errno。
         * @note  一个巧妙的用法是：lseek(fd, 0, SEEK_END) 可以直接返回整个文件的大小。
         */
        size_t file_size = ::lseek(file_fd, 0, SEEK_END); // 获取文件大小
        ::lseek(file_fd, 0, SEEK_SET); // 重置文件偏移

        // 把文件任务放到发送队列里，等待 handleWrite() 处理
        outputQueue_.push(OutputItem(file_fd, file_size));
        if (outputQueue_.size() == 1) { // 如果之前队列是空的，说明 handleWrite() 没有在工作，需要踢一脚它了
            handleWrite(); // 直接调用一次 handleWrite()，尝试把文件发送出去，后续如果没发完会自动注册 EPOLLOUT 继续发送
        }
    }

    void TcpConnection::handleWrite() {
        if (state_ != StateE::kConnected) return; // 连接已断开，不再处理写事件
        auto guard = shared_from_this();
        while (!outputQueue_.empty()) {
            OutputItem& item = outputQueue_.front();
            if (item.is_file) {
                /**
                 * @brief 在两个文件描述符之间直接传递数据 (零拷贝核心系统调用)
                 * @signature ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
                 * @param out_fd 目标文件描述符，数据将写入此处 (此处为客户端的 TCP 套接字 fd_)
                 * @param in_fd  源文件描述符，数据将从此处读取 (此处为刚刚打开的本地文件 file_fd)
                 * @param offset 指向读写偏移量变量的指针。表示从 in_fd 的哪个位置开始读。
                 *               调用返回时，内核会自动将该变量累加实际发送的字节数！(这完美契合了我们的异步断点续传需求)
                 * @param count  本次尝试发送的字节数 (传入还剩多少字节没发：file_size - file_offset)
                 * @return 成功返回实际发送的字节数；如果非阻塞套接字的内核发送缓冲区满了，返回 -1 并自动设置 errno 为 EAGAIN 或 EWOULDBLOCK
                 * @note 神奇之处：数据从磁盘读到内核页缓存后，直接由网卡 DMA 扒走发送。全程没有跨越内核态去把数据拷贝到你的 C++ 变量里，这就是实现 10 万并发下载的秘密。
                 */
                ssize_t bytes_sent = sendfile(fd_, item.file_fd, &item.file_offset, item.file_size - item.file_offset);
                if (bytes_sent > 0) {
                    // 检查是否发送完成
                    if (static_cast<size_t>(bytes_sent) == item.file_size - static_cast<size_t>(item.file_offset)) {
                        // 文件发送完成，关闭文件描述符并从队列中移除
                        ::close(item.file_fd);
                        outputQueue_.pop();
                    } // 否则，bytes_sent 已经自动累加到 item.file_offset 了，下一轮循环会继续发送剩余部分
                } else {
                    if (bytes_sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        // 内核发送缓冲区满了，暂时无法继续发送，等待下一次可写事件
                        break;
                    } else {
                        LOG_ERROR << "发送文件失败!";
                        ::close(item.file_fd); // 发生错误也要关闭文件描述符
                        outputQueue_.pop(); // 从队列中移除这个任务
                    }
                }
            } else {
                int bytes = ::send(fd_, item.str_data.c_str(), item.str_data.size(), 0);
                if (bytes > 0) {
                    // 检查字符串是否被完整发送
                    if (bytes == static_cast<int>(item.str_data.size())) {
                        // 字符串发送完成，从队列中移除
                        outputQueue_.pop();
                    } else { // 更新 str_data，继续发送剩余部分
                        item.str_data = item.str_data.substr(bytes);
                    }
                } else {
                    if (bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        // 内核发送缓冲区满了，暂时无法继续发送，等待下一次可写事件
                        break;
                    } else {
                        LOG_ERROR << "发送数据失败!";
                        outputQueue_.pop(); // 从队列中移除这个任务
                    }
                }
            }
        }

        // 如果发送队列已经空了，说明 handleWrite() 的工作完成了，可以暂时休息了，取消对 EPOLLOUT 的监听
        if (outputQueue_.empty()) {
            channel_->disableWriting();
        } else { // 说明此时缓冲区还有数据没发完，需要继续监听 EPOLLOUT 事件
            channel_->enableWriting();
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
        if (state_ != StateE::kConnected) return; // 已经在关闭了，不重复处理
        state_ = StateE::kDisconnecting;
        channel_->disableAll(); // 立即从 epoll 中注销，防止后续事件重入
        // 触发关闭回调，TcpServer 会负责把它从账本里删掉，并销毁堆内存
        auto guard = shared_from_this();
        if (closeCallback_) {
            closeCallback_(guard);
        }
    }

    void TcpConnection::forceClose() {
        if (state_ == StateE::kConnected) {
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
}