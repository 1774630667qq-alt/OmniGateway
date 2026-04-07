/*
 * @Author: error: error: git config user.name & please set dead value or
 * install git && error: git config user.email & please set dead value or
 * install git & please set dead value or install git
 * @Date: 2026-04-05 22:42:07
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * install git && error: git config user.email & please set dead value or
 * install git & please set dead value or install git
 * @LastEditTime: 2026-04-06 19:07:52
 * @FilePath: /OmniGateway/include/InetAddress.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <string>

namespace MyServer {

/**
 * @class InetAddress
 * @brief 网际地址类：封装了 sockaddr_in 结构体
 */
class InetAddress {
public:
    // 构造函数：通常用于 Connector 发起连接
    // 将 "117.72.11.81" 和 8080 封装进底层结构体
    explicit InetAddress(uint16_t port = 8080, std::string ip = "127.0.0.1");

    // 构造函数：通常用于 Acceptor 接收连接后，包装内核返回的地址
    explicit InetAddress(const sockaddr_in& addr) : addr_(addr) {}

    explicit InetAddress(const InetAddress& other) : addr_(other.addr_) {}

    // 返回 IP 字符串 (如 "192.168.1.1")
    std::string toIp() const;

    // 返回端口号 (主机字节序)
    uint16_t toPort() const;

    // 获取底层的 C 结构体指针 (供系统调用使用)
    const sockaddr_in* getSockAddr() const { return &addr_; }

    socklen_t getSockAddrLen() const { return sizeof(addr_); }

    void setSockAddr(const sockaddr_in& addr) { addr_ = addr; }

private:
    // 真正存储地址的地方：Linux 系统定义的 IPv4 地址结构体
    struct sockaddr_in addr_; 
};

} // namespace MyServer