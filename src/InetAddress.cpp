/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-06 18:13:31
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-06 18:33:08
 * @FilePath: /OmniGateway/src/InetAddress.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "InetAddress.hpp"
#include <arpa/inet.h>
#include <Logger.hpp>

namespace MyServer {

InetAddress::InetAddress(uint16_t port, std::string ip) {
    addr_.sin_family = AF_INET;   // IPv4
    addr_.sin_port = htons(port); // 主机字节序 -> 网络字节序

    /**
    * @brief 将 IP 地址从文本字符串转换为网络字节序的二进制形式 (Presentation to Network)
    * @signature int inet_pton(int af, const char *src, void *dst)
    * @param af 地址族（Address Family），此处使用 AF_INET 表示 IPv4
    * @param src 指向包含点分十进制 IP 字符串的指针
    * @param dst 指向存储转换后二进制地址的缓冲区（通常是 struct in_addr）
    * @return 1 表示转换成功；0 表示输入格式无效；-1 表示地址族不支持（errno 会被设置）
    */
    if (inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
        // 如果 IP 地址无效，默认监听所有网卡
        addr_.sin_addr.s_addr = htonl(INADDR_ANY);
    }
}

std::string InetAddress::toIp() const {
    char buf[INET_ADDRSTRLEN];
    /**
    * @brief 将网络字节序的二进制地址转换为文本字符串 (Network to Presentation)
    * @signature const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
    * @param af 地址族，此处使用 AF_INET 表示 IPv4
    * @param src 指向网络字节序二进制地址的指针 (struct in_addr)
    * @param dst 指向用于存储结果的可写字符串缓冲区的指针
    * @param size 目标缓冲区的大小 (INET_ADDRSTRLEN)
    * @return 成功返回指向 dst 的非空指针；失败返回 NULL (errno 会被设置)
    */
    if (inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf)) == nullptr) {
        LOG_ERROR << "地址转换失败";
        return "";
    }
    return std::string(buf);
}

uint16_t InetAddress::toPort() const {
    // 网络字节序 -> 主机字节序
    return ntohs(addr_.sin_port);
}

} // namespace MyServer