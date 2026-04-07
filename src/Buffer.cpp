/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-07 17:21:00
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-07 17:21:00
 * @FilePath: /OmniGateway/src/Buffer.cpp
 * @Description: Buffer 核心 I/O 实现：readFd 使用 readv + scatter/gather IO
 */
#include "Buffer.hpp"
#include <errno.h>
#include <sys/uio.h> // readv

namespace MyServer {

/// HTTP 头部结束标志 "\r\n\r\n"
const char Buffer::kCRLF[] = "\r\n\r\n";

/**
 * @brief 从非阻塞套接字中高效读取数据
 * @signature ssize_t Buffer::readFd(int fd, int* savedErrno)
 * @param fd 非阻塞套接字文件描述符
 * @param savedErrno 出参，保存最后一次 read 失败时的 errno
 * @return 本次总共读取的字节数；返回 0 表示对端关闭；返回 -1 表示不可恢复的错误
 *
 * @details 核心设计思想 —— Scatter/Gather IO + ET 循环榨干：
 *
 * 由于底层 Channel 使用 EPOLLET（边缘触发）模式，必须在一次可读事件中
 * 把内核接收缓冲区的数据全部抽干，否则会导致数据丢失。
 *
 * 每次 readv 调用配置两块 iovec：
 *   iov[0] → Buffer 自身的可写区域（堆内存，随 Buffer 生命周期持续存在）
 *   iov[1] → 栈上 65536 字节的 extrabuf（临时高速缓冲，函数返回时自动释放）
 *
 * 这样做的妙处：
 *   - 大多数情况下，数据直接落入 iov[0]，零额外拷贝
 *   - 只有当数据量超过 Buffer 当前可写空间时，溢出部分才会落入 extrabuf
 *   - 事后再通过 append() 把 extrabuf 中的数据追加进来（触发智能扩容）
 *   - 避免了每次 readFd 前都预先 resize 一个超大 Buffer 的内存浪费
 */
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    // 栈上临时缓冲区，64KB，用于承接 Buffer 本身空间不足时的溢出数据
    char extrabuf[65536];
    // 本次 readFd 调用的累计读取字节数
    ssize_t totalBytes = 0;

    while (true) {
        /**
         * @brief 配置 iovec 结构体数组，实现分散读入（Scatter Read）
         *
         * struct iovec {
         *     void  *iov_base;  // 缓冲区起始地址
         *     size_t iov_len;   // 缓冲区长度
         * };
         *
         * iov[0]: 指向 Buffer 内部的可写区域 → 数据优先落入堆内存
         * iov[1]: 指向栈上的 extrabuf        → 溢出数据的临时着陆区
         */
        struct iovec vec[2];
        const size_t writable = writableBytes();

        vec[0].iov_base = beginWrite();
        vec[0].iov_len = writable;
        vec[1].iov_base = extrabuf;
        vec[1].iov_len = sizeof(extrabuf);

        // 如果 Buffer 自身可写空间已经 >= 64KB，就不需要 extrabuf 了
        const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

        /**
         * @brief 执行聚合读取（Scatter Read 系统调用）
         * @signature ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
         * @param fd     非阻塞套接字文件描述符
         * @param iov    iovec 结构体数组，描述多个不连续的目标缓冲区
         * @param iovcnt iov 数组中有效元素的数量
         * @return 成功返回实际读取的总字节数；返回 0 表示对端关闭连接；
         *         返回 -1 表示出错（errno == EAGAIN 表示内核缓冲区已抽干）
         */
        const ssize_t n = ::readv(fd, vec, iovcnt);

        if (n < 0) {
            // 读取出错
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // ET 模式下表示内核接收缓冲区已被彻底抽干，正常退出循环
                break;
            } else {
                // 不可恢复的错误（如 ECONNRESET）
                *savedErrno = errno;
                return -1;
            }
        } else if (n == 0) {
            // 对端关闭了连接（FIN）
            // 如果之前已经读到了一些数据，先返回已读字节数
            // 上层会在下一次 handleRead 中收到 n==0 并触发关闭流程
            if (totalBytes > 0) {
                break;
            }
            return 0;
        } else {
            // 成功读取了 n 字节
            if (static_cast<size_t>(n) <= writable) {
                // 全部数据都落入了 Buffer 自身的可写区域，只需推进写游标
                writerIndex_ += n;
            } else {
                // 数据溢出到了 extrabuf：
                // 1. 先将 writerIndex_ 推到容器末尾极限（iov[0] 部分已满）
                writerIndex_ = buf_.size();
                // 2. 将 extrabuf 中的残留数据追加到 Buffer（触发 ensureWritableBytes → makeSpace）
                append(extrabuf, n - writable);
            }
            totalBytes += n;
        }
    }

    return totalBytes;
}

} // namespace MyServer
