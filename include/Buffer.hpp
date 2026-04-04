/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-22 20:28:39
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-31 14:20:22
 * @FilePath: /ServerPractice/include/Buffer.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <string>

namespace MyServer {

/**
 * @brief 应用层缓冲区：解决 TCP 粘包/半包问题的神器
 */
class Buffer {
private:
    std::string buf_; // 这里用 string 极其方便，因为它自带动态扩容和各种查找方法

public:
    Buffer() = default;
    ~Buffer() = default;

    /**
     * @brief 把底层收到的数据追加到缓冲区
     */
    void append(const char* data, size_t len) {
        buf_.append(data, len);
    }

    /**
     * @brief 获取当前缓冲区里有多少字节的数据
     */
    size_t size() const {
        return buf_.size();
    }

    /**
     * @brief 获取底层缓冲区的首地址 (用于 send)
     */
    const char* data() const {
        return buf_.data();
    }

    /**
     * @brief 从缓冲区头部删掉指定长度的数据 (发送成功后调用)
     */
    void retrieve(size_t len) {
        if (len <= buf_.size()) {
            buf_.erase(0, len);
        }
    }

    /**
     * @brief 从当前的缓冲区中http数据包中找到第一个\r\n\r\n
     */
    size_t findCRLF() const {
        return buf_.find("\r\n\r\n");
    }

    /**
     * @brief 提取出一条完整的数据 (这需要根据你的协议来写)
     * * 假设我们的协议是：每条消息以 \n 结尾
     * @return 如果找到了一完整消息，返回它并从 Buffer 中删掉；如果不够一条，返回空字符串
     */
    std::string extractMessage() {
        // 在缓冲区中查找 '\n' 的位置
        size_t pos = buf_.find('\n');
        
        if (pos != std::string::npos) {
            // 找到了！说明有一条完整的消息
            // 提取从头到 '\n' 的所有字符 (包含 \n)
            std::string msg = buf_.substr(0, pos + 1); 
            // 把提取走的数据从水池里删掉
            buf_.erase(0, pos + 1); 
            return msg;
        }
        
        // 没找到 \n，说明是半包，什么都不返回，继续等后续数据
        return ""; 
    }

    /**
     * @brief 专门用于提取 HTTP 头部的解析器
     * @return 如果找到了 \r\n\r\n，就返回整个 HTTP 头部字符串；否则返回空字符串继续等待。
     */
    std::string extractHttpHeaders() {
        // 在缓冲区中查找 HTTP 头部的结束标志 "\r\n\r\n"
        size_t pos = buf_.find("\r\n\r\n");
        
        if (pos != std::string::npos) {
            // 找到了！提取出包含 \r\n\r\n 在内的完整 HTTP 头部
            size_t header_len = pos + 4; // "\r\n\r\n" 占用 4 个字节
            std::string header = buf_.substr(0, header_len); 
            // 由于可能出现半包的情况，我们暂时不从 Buffer 中删除数据，等业务层解析完 HTTP 头部后再决定要不要删掉。
            // buf_.erase(0, header_len); 
            return header;
        }
        
        // 没找到 \r\n\r\n，说明 HTTP 请求还没收全，什么都不返回
        return ""; 
    }
};

} // namespace MyServer