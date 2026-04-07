/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-22 20:28:39
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-07 17:21:00
 * @FilePath: /OmniGateway/include/Buffer.hpp
 * @Description: 工业级动态缓冲区，基于 vector<char> + 双游标设计
 */
#pragma once
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace MyServer {

/**
 * @brief 工业级应用层动态缓冲区
 * @details 基于 std::vector<char> + 双游标（readerIndex_ / writerIndex_）设计，
 *          解决 TCP 粘包/半包问题，并支持 O(1) 前置插入和智能内存碎片合并。
 *
 * 内部内存布局：
 * +-------------------+------------------+------------------+
 * | prependable bytes |  readable bytes  |  writable bytes  |
 * |                   |     (CONTENT)    |                  |
 * +-------------------+------------------+------------------+
 * |                   |                  |                  |
 * 0      <=     readerIndex_   <=   writerIndex_    <=    size()
 *
 * prependable = readerIndex_                         (含固定的 kCheapPrepend)
 * readable    = writerIndex_ - readerIndex_
 * writable    = size() - writerIndex_
 */
class Buffer {
public:
    /// 预留头部大小（8 字节），用于 O(1) 前置插入协议长度字段等
    static const size_t kCheapPrepend = 8;
    /// 数据区的初始容量
    static const size_t kInitialSize = 1024;

    /**
     * @brief 构造函数：分配初始内存，双游标归位到预留头部末尾
     * @param initialSize 数据区初始大小，默认 1024 字节
     */
    explicit Buffer(size_t initialSize = kInitialSize)
        : buf_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    ~Buffer() = default;

    // ====================== 容量查询接口 ======================

    /**
     * @brief 返回当前有效可读字节数
     * @return writerIndex_ - readerIndex_
     */
    size_t readableBytes() const { return writerIndex_ - readerIndex_; }

    /**
     * @brief 返回尾部剩余连续可写字节数
     * @return buf_.size() - writerIndex_
     */
    size_t writableBytes() const { return buf_.size() - writerIndex_; }

    /**
     * @brief 返回头部预留空间大小（含已消费的废弃空间）
     * @return readerIndex_
     */
    size_t prependableBytes() const { return readerIndex_; }

    // ====================== 数据读取接口 ======================

    /**
     * @brief 返回可读数据的起始地址（只读窥视，不移动游标）
     * @return 指向 readerIndex_ 位置的 const 指针
     */
    const char* peek() const { return begin() + readerIndex_; }

    /**
     * @brief 在可读区域中查找 "\r\n\r\n" 的位置
     * @return 找到则返回 "\r\n\r\n" 相对于 peek() 的偏移量；未找到返回 std::string::npos
     */
    size_t findCRLF() const {
        const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 4);
        return crlf == beginWrite() ? std::string::npos
                                    : static_cast<size_t>(crlf - peek());
    }

    // ====================== 数据消费接口 ======================

    /**
     * @brief 声明消费 len 字节数据，将 readerIndex_ 向后推进
     * @param len 要消费的字节数，不得超过 readableBytes()
     */
    void retrieve(size_t len) {
        assert(len <= readableBytes());
        if (len < readableBytes()) {
            readerIndex_ += len;
        } else {
            retrieveAll();
        }
    }

    /**
     * @brief 消费所有有效数据，双游标归位
     */
    void retrieveAll() {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    /**
     * @brief 消费所有有效数据并封装为 std::string 返回
     * @return 包含全部可读数据的字符串
     */
    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }

    /**
     * @brief 消费指定长度的数据并封装为 std::string 返回
     * @param len 要消费的字节数
     * @return 包含指定长度可读数据的字符串
     */
    std::string retrieveAsString(size_t len) {
        assert(len <= readableBytes());
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    // ====================== 数据写入接口 ======================

    /**
     * @brief 追加数据到缓冲区尾部
     * @param data 数据源地址
     * @param len 数据长度
     * @details 自动处理容量不足的情况：优先内部腾挪碎片合并，不足则物理扩容
     */
    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        std::copy(data, data + len, beginWrite());
        hasWritten(len);
    }

    /**
     * @brief 追加 std::string 到缓冲区尾部
     * @param str 要追加的字符串
     */
    void append(const std::string& str) {
        append(str.data(), str.size());
    }

    /**
     * @brief 在预留头部区域前置插入数据（O(1) 复杂度）
     * @param data 数据源地址
     * @param len 数据长度，不得超过 prependableBytes()
     */
    void prepend(const void* data, size_t len) {
        assert(len <= prependableBytes());
        readerIndex_ -= len;
        const char* d = static_cast<const char*>(data);
        std::copy(d, d + len, begin() + readerIndex_);
    }

    // ====================== 核心 I/O 接口 ======================

    /**
     * @brief 从非阻塞套接字中高效读取数据（Scatter/Gather IO + ET 循环榨干）
     * @param fd 非阻塞套接字文件描述符
     * @param savedErrno 出参，保存最后一次 read 失败时的 errno
     * @return 本次 readFd 总共读取的字节数；返回 0 表示对端关闭；返回 -1 表示出错
     */
    ssize_t readFd(int fd, int* savedErrno);

    // ====================== 内部辅助方法 ======================

    /**
     * @brief 返回可写区域的起始地址
     */
    char* beginWrite() { return begin() + writerIndex_; }

    /**
     * @brief 返回可写区域的起始地址（const 版本）
     */
    const char* beginWrite() const { return begin() + writerIndex_; }

    /**
     * @brief 确认已写入 len 字节，推进 writerIndex_
     * @param len 实际写入的字节数
     */
    void hasWritten(size_t len) {
        assert(len <= writableBytes());
        writerIndex_ += len;
    }

private:
    /// HTTP 头部结束标志
    static const char kCRLF[];

    /**
     * @brief 返回 vector 底层数组首地址
     */
    char* begin() { return &*buf_.begin(); }

    /**
     * @brief 返回 vector 底层数组首地址（const 版本）
     */
    const char* begin() const { return &*buf_.begin(); }

    /**
     * @brief 确保至少有 len 字节的连续可写空间
     * @param len 需要的最小可写字节数
     * @details 内存管理策略：
     *   1. 若尾部空间 >= len，无需任何操作
     *   2. 若 (头部废弃空间 + 尾部空间) >= len，执行内部腾挪（碎片合并）
     *   3. 否则，调用 vector::resize() 物理扩容
     */
    void makeSpace(size_t len) {
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
            // 整体空间（含已废弃区域）仍然不够，必须物理扩容
            buf_.resize(writerIndex_ + len);
        } else {
            // 整体空间充足，执行内部腾挪：将有效数据前移到紧接 kCheapPrepend 之后
            size_t readable = readableBytes();
            std::copy(begin() + readerIndex_,
                      begin() + writerIndex_,
                      begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
            assert(readable == readableBytes());
        }
    }

    /**
     * @brief 确保有足够的可写空间，空间不足时触发 makeSpace
     * @param len 需要的最小可写字节数
     */
    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len) {
            makeSpace(len);
        }
        assert(writableBytes() >= len);
    }

    std::vector<char> buf_;   ///< 底层连续内存容器
    size_t readerIndex_;      ///< 读游标：指向第一个可读字节
    size_t writerIndex_;      ///< 写游标：指向第一个可写字节
};

} // namespace MyServer