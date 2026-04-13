/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-06 18:34:53
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-12 22:08:31
 * @FilePath: /OmniGateway/src/Connector.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "Connector.hpp"
#include "Channel.hpp"
#include "Logger.hpp"
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace MyServer {
Connector::Connector(EventLoop *loop, const InetAddress &serverAddr)
    : loop_(loop), serverAddr_(serverAddr) {}

Connector::~Connector() { stop(); }

void Connector::start() {
  state_ = kConnecting;

  auto self = shared_from_this();

  loop_->queueInLoop([self]() { self->connect(); });
}

void Connector::stop() {
    state_ = kDisconnected;
    if (channel_) {
      channel_->disableAll();
      channel_->remove();
      channel_.reset();
    }
}

void Connector::connect() {
    /**
    * @brief 创建一个用于通信的端点 (Socket)
    * @signature int socket(int domain, int type, int protocol)
    * @param domain 通信的协议族 (Protocol Family / Domain)：
    *        - AF_INET: IPv4 网络协议
    *        - AF_INET6: IPv6 网络协议
    *        - AF_UNIX / AF_LOCAL: 本地进程间通信 (IPC)
    * @param type 套接字的类型及可选标志位，常用选项可使用按位或 (|) 组合：
    *        [基础类型]
    *        - SOCK_STREAM: 提供面向连接的、可靠的字节流服务 (如 TCP)
    *        - SOCK_DGRAM: 提供无连接的、不可靠的数据报服务 (如 UDP)
    *        [附加标志位 (Linux 扩展)]
    *        - SOCK_NONBLOCK: 将套接字设为非阻塞模式。调用 accept/connect/读写
    * 时不会阻塞当前线程
    *        - SOCK_CLOEXEC: 设置 close-on-exec 标志。在子进程中执行 exec()
    * 时自动关闭该套接字，防止 fd 泄漏
    * @param protocol 指定特定的协议代码。通常设为 0，表示使用由 domain 和 type
    * 组合决定的默认协议 (例如 TCP)
    * @return 成功时返回新创建的套接字文件描述符 (非负整数)；失败时返回 -1
    * (并设置 errno)
    */
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0) {
      LOG_ERROR << "socket() 创建失败: " << strerror(errno);
      return;
    }

    /**
    * @brief 启动一个套接字连接 (Initiate a connection on a socket)
    * @signature int connect(int sockfd, const struct sockaddr *addr, socklen_t
    * addrlen)
    * @param sockfd 套接字文件描述符
    * @param addr 目标地址结构体指针。由于 connect 是通用的，需将特定的地址结构
    * (如 sockaddr_in) 强转为通用的 sockaddr* 类型。内核会根据 addrlen 和 addr
    * 内的 sa_family 成员来解析具体地址。
    * @param addrlen 地址结构体的长度 (sizeof(struct sockaddr_in))
    * @return
    *        - 0: 连接立即成功完成 (通常见于本地回环或已经就绪的情况)
    *        - -1: 出错，需检查 errno。在 **非阻塞模式 (SOCK_NONBLOCK)**
    * 下，常见错误码：
    *          - [EINPROGRESS]: 连接正在异步建立中。这是非阻塞 connect
    * 的正常预期行为， 后续需通过 poll/epoll 监听该 fd
    * 的写事件来判断连接是否完成。
    *          - [EISCONN]: 套接字已经连接。
    *          - [ECONNREFUSED]: 目标地址拒绝连接。
    *          - [ETIMEDOUT]: 连接超时。
    *          - [EADDRNOTAVAIL]: 无法分配请求的地址。
    */
    int ret = ::connect(sockfd, (sockaddr *)serverAddr_.getSockAddr(),
                        serverAddr_.getSockAddrLen());
    int savedErrno = (ret == -1) ? errno : 0;
    // 使用 weak_ptr 打破 Connector → Channel → shared_ptr<Connector> 的循环引用
    std::weak_ptr<Connector> weak = shared_from_this();
    switch (savedErrno) {
    // ────────── 连接正常或正在建立中，挂载 Channel 监听可写事件 ──────────
    case 0:           // connect() 立即成功 (常见于本地回环连接)
    case EINPROGRESS: // TCP 三次握手正在异步进行中 —— 非阻塞 connect
                      // 的正常预期返回值
    case EINTR:   // connect() 被信号中断，但连接可能仍在继续建立
    case EISCONN: // 套接字已处于连接状态 (重复 connect 时可能触发)
      channel_ = std::make_unique<Channel>(loop_, sockfd);
      channel_->setWriteCallback([weak]() {
        if (auto self = weak.lock()) self->handleWrite();
      });
      channel_->setCloseCallback([weak]() {
        if (auto self = weak.lock()) self->handleError();
      });
      channel_->enableWriting();
      break;

    // ────────── 可恢复的临时性错误，关闭当前 fd 后延迟重连 ──────────
    case EAGAIN: // 临时端口资源耗尽，无法分配本地端口号给该连接
    case EADDRINUSE: // 指定的本地地址/端口组合已被占用 (TIME_WAIT 状态等)
    case EADDRNOTAVAIL: // 请求的本地地址在本机网络接口上不可用
    case ECONNREFUSED: // 目标主机可达，但目标端口没有进程在监听 (收到 RST)
    case ENETUNREACH: // 目标网络不可达 —— 路由表中无法找到通往目标网络的路径
      retry(sockfd, savedErrno);
      break;

    // ────────── 不可恢复的致命错误，放弃连接 ──────────
    case EACCES: // 权限不足：尝试连接广播地址但未设置 SO_BROADCAST，或受
                // SELinux/防火墙拦截
    case EPERM: // 操作被禁止：防火墙规则阻止了该连接
    case EAFNOSUPPORT: // 地址族不支持：sockaddr 中的地址族与套接字不匹配
    case EALREADY: // 先前的非阻塞 connect 仍在进行中 (不应出现在本状态机中)
    case EBADF: // 无效的文件描述符 (程序逻辑错误)
    case EFAULT: // addr 指针指向用户空间不可访问的内存 (程序逻辑错误)
    case ENOTSOCK: // sockfd 不是套接字 (程序逻辑错误)
    default:
      LOG_ERROR << "connect() 遇到不可恢复错误: errno=" << savedErrno << " ("
                << strerror(savedErrno) << ")";
      ::close(sockfd);
      break;
    }
}

void Connector::retry(int sockfd, int savedErrno) {
    ::close(sockfd);

    if (state_ == kConnected || state_ == kDisconnected) {
      return;
    }

    LOG_WARNING << "connect() 遇到可恢复错误 (errno=" << savedErrno << " "
                << strerror(savedErrno) << ")，将在 " << retryDelayMs_
                << "ms 后重试";

    // 先用当前延迟调度重连，再翻倍 (指数退避，上限 30s)
    loop_->runAfter(retryDelayMs_,
                  [self = shared_from_this()]() { self->connect(); });
    retryDelayMs_ = std::min(retryDelayMs_ * 2, 30000);
}

void Connector::handleWrite() {
    if (state_ != kConnecting || !channel_)
      return;

    // 先提取 fd，然后彻底释放 Channel 的所有权
    // 防止后续析构/stop 时对已交出的 fd 进行重复操作
    int sockfd = channel_->getFd();
    channel_->disableAll();
    channel_->remove();
    // 延迟销毁 channel_，防止在 Channel::handleEvent 尚未结束时销毁自身 (导致 core dump)
    Channel* ch = channel_.release();
    loop_->queueInLoop([ch]() {
        delete ch;
    });

    // 通过 getsockopt 验证非阻塞 connect 是否真正成功
    int err = 0;
    socklen_t errLen = sizeof(err);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errLen) < 0) {
      err = errno;
      LOG_ERROR << "getsockopt() 获取错误码失败: " << strerror(errno);
    }

    if (err == 0) {
      // 连接成功，将 fd 移交给上层
      state_ = kConnected;
      if (newConnectionCallback_) {
        newConnectionCallback_(sockfd);
      }
    } else {
      // 连接失败 (如 ECONNREFUSED)，触发指数退避重连
      LOG_WARNING << "非阻塞 connect 最终失败: " << strerror(err);
      retry(sockfd, err);
    }
}

void Connector::handleError() {
    if (state_ != kConnecting || !channel_)
      return;

    // 先提取 fd 和错误信息，然后释放 Channel
    int sockfd = channel_->getFd();
    channel_->disableAll();
    channel_->remove();
    // 延迟销毁 channel_，防止在 Channel::handleEvent 尚未结束时销毁自身
    Channel* ch = channel_.release();
    loop_->queueInLoop([ch]() {
        delete ch;
    });

    int err = 0;
    socklen_t errLen = sizeof(err);
    ::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errLen);

    LOG_ERROR << "Connector::handleError() SO_ERROR=" << err << " ("
              << strerror(err) << ")";

    retry(sockfd, err);
}
} // namespace MyServer