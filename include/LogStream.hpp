#pragma once
#include <string>
#include <string.h> // for memcpy

namespace MyServer {

const int kSmallBuffer = 4000;         // 4KB 单条日志的默认缓冲区大小
const int kLargeBuffer = 4000 * 1000;  // 4MB 后台大缓冲区大小 (后面 AsyncLogging 会用到)

/**
 * @brief 定长缓冲区模板类
 * @details 为什么不用 std::string？因为 std::string 会动态扩容，带来极大的性能开销。
 * 我们预先分配好一块内存（比如 4KB），像写数组一样往里面塞数据，速度逼近物理极限。
 */
template<int SIZE>
class FixedBuffer {
private:
    char data_[SIZE]; ///< 真正存储数据的连续内存
    char* cur_;       ///< 指向当前可以写入的空闲位置的指针

public:
    FixedBuffer() : cur_(data_) {}
    ~FixedBuffer() = default;

    /**
     * @brief 向缓冲区追加数据
     * @param buf 要追加的数据首地址
     * @param len 要追加的数据长度
     */
    void append(const char* buf, size_t len) {
        if (static_cast<size_t>(avail()) >= len) {
            memcpy(cur_, buf, len);
            cur_ += len;
        } else { // 截断数据，剩余部分抛弃
            size_t avail_len = avail();
            memcpy(cur_, buf, avail_len);
            cur_ += avail_len;
        }
    }

    // --- 以下是已经为你写好的极速辅助方法 ---
    const char* data() const { return data_; }
    int length() const { return static_cast<int>(cur_ - data_); }

    // 计算当前还剩多少空间可以写
    int avail() const { return static_cast<int>(end() - cur_); }

    // 清空缓冲区：极其精妙！只需要把指针拨回头部，不需要 memset 清零内存！开销为 0！
    void reset() { cur_ = data_; }

    // 彻底清空缓冲区：把内存清零，并把指针拨回头部。开销较大，慎用！
    void bzero() { ::bzero(data_, sizeof(data_)); }

private:
    const char* end() const { return data_ + sizeof(data_); }
};

/**
 * @brief 极速日志格式化工具类
 * @details 重载 << 运算符，把各种类型的数据极速地转换成字符，并追加到内部的 FixedBuffer 中。
 */
class LogStream {
public:
    using Buffer = FixedBuffer<kSmallBuffer>;

private:
    Buffer buffer_; ///< 内部的定长缓冲区（小垃圾桶）
    static const int kMaxNumericSize = 48; ///< 数字转字符串的最大长度

public:
    LogStream() = default;
    ~LogStream() = default;

    // --- 以下方法将在 src/LogStream.cpp 中实现 ---

    // 接收布尔值
    LogStream& operator<<(bool v);

    // 接收各种整型
    LogStream& operator<<(short);
    LogStream& operator<<(unsigned short);
    LogStream& operator<<(int);
    LogStream& operator<<(unsigned int);
    LogStream& operator<<(long);
    LogStream& operator<<(unsigned long);
    LogStream& operator<<(long long);
    LogStream& operator<<(unsigned long long);

    // 接收浮点型
    LogStream& operator<<(float v);
    LogStream& operator<<(double v);

    // 接收字符和字符串
    LogStream& operator<<(char v);
    LogStream& operator<<(const char* str);
    LogStream& operator<<(const std::string& v);

    // 允许直接追加原始数据，避免在外部格式化后调用 operator<< 带来的额外开销
    void append(const char* data, int len) { buffer_.append(data, len); }

    // 暴露底层的 Buffer 给未来的异步日志大管家使用
    const Buffer& buffer() const { return buffer_; }
    void resetBuffer() { buffer_.reset(); }
};

} // namespace MyServer