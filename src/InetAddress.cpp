/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-06 18:13:31
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-14 09:35:52
 * @FilePath: /OmniGateway/src/InetAddress.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "InetAddress.hpp"
#include <arpa/inet.h>
#include <Logger.hpp>
#include <cstring>
#include <netdb.h>
#include <sys/types.h>

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

bool InetAddress::resolve(const std::string& hostname, std::string& OutIp) {
    /**
     * @brief DNS 解析结果的核心数据结构，既用作查询条件（hints），也用作返回结果链表的节点
     * @details struct addrinfo 是 POSIX 标准定义的地址信息结构体，由 getaddrinfo() 分配并填充。
     *          其核心字段如下：
     *          - ai_flags:    控制解析行为的标志位（如 AI_PASSIVE 用于服务器绑定，AI_NUMERICHOST 禁止 DNS 查询）
     *          - ai_family:   地址族（AF_INET = IPv4, AF_INET6 = IPv6, AF_UNSPEC = 不限）
     *          - ai_socktype: 套接字类型（SOCK_STREAM = TCP, SOCK_DGRAM = UDP）
     *          - ai_protocol: 协议编号（通常为 0 表示自动选择）
     *          - ai_addrlen:  ai_addr 指向的地址结构体的字节长度
     *          - ai_addr:     指向 sockaddr 的通用指针，实际类型取决于 ai_family（IPv4 时为 sockaddr_in）
     *          - ai_canonname: 主机的规范名称（仅在 hints.ai_flags 设置 AI_CANONNAME 时填充）
     *          - ai_next:     指向链表中下一个 addrinfo 节点（同一主机可能有多个地址，如多网卡或双栈）
     *
     *          作为 hints 参数时：仅 ai_flags/ai_family/ai_socktype/ai_protocol 有效，其余字段必须为 0 或 NULL。
     *          作为结果链表时：所有字段均由 getaddrinfo() 自动填充，通过 ai_next 遍历链表即可获取全部解析结果。
     * @note 结果链表由 getaddrinfo() 内部动态分配，使用完毕后必须调用 freeaddrinfo() 释放。
     * @see getaddrinfo, freeaddrinfo
     */
    struct addrinfo hints;
    struct addrinfo* res;
    struct addrinfo* p;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      // 只查询 IPv4 地址
    hints.ai_socktype = SOCK_STREAM; // 只查询 TCP 流
    
    /**
     * @brief 执行主机名到网络地址的 DNS 解析，返回可直接用于 connect()/bind() 的地址信息链表
     * @signature int getaddrinfo(const char *node, const char *service,
     *                            const struct addrinfo *hints, struct addrinfo **res)
     * @param node 待解析的主机名（如 "api.edgefn.net"）或数字地址字符串（如 "192.168.1.1"）。
     *             传入主机名时将触发 DNS 查询（可能涉及网络 I/O，存在阻塞风险）；
     *             传入数字地址时仅做格式转换，不产生网络请求
     * @param service 服务名（如 "http"）或端口号字符串（如 "443"），用于填充返回地址中的端口字段。
     *                传入 nullptr 时端口保持未初始化
     * @param hints 指向 addrinfo 的过滤条件：限定返回结果的地址族、套接字类型等。
     *              本项目中设置 ai_family=AF_INET + ai_socktype=SOCK_STREAM，
     *              意味着只返回 "IPv4 + TCP" 的解析结果
     * @param res 输出参数，指向 getaddrinfo() 内部动态分配的结果链表头指针。
     *            一次 DNS 查询可能返回多个 A 记录，每个 A 记录对应链表中的一个 addrinfo 节点
     * @return 0 表示成功；非零为 EAI_* 系列错误码，常见值包括：
     *         - EAI_NONAME: 主机名或服务名未知
     *         - EAI_AGAIN:  DNS 服务器返回临时失败，应稍后重试
     *         - EAI_FAIL:   DNS 服务器返回永久失败
     *         - EAI_MEMORY: 内存分配失败
     *         - EAI_SYSTEM: 系统调用错误，具体原因存储在 errno 中
     * @note 该函数是线程安全的（MT-Safe），是 gethostbyname() 的现代替代品。
     *       但它是同步阻塞的，DNS 查询可能耗时数秒，不适合在 EventLoop 线程中直接调用。
     * @see freeaddrinfo, gai_strerror, gethostbyname
     */
    int status = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
    if (status != 0) {
        /**
         * @brief 将 getaddrinfo() 返回的 EAI_* 错误码转换为人类可读的错误描述字符串
         * @signature const char *gai_strerror(int errcode)
         * @param errcode getaddrinfo() 或 getnameinfo() 返回的非零错误码（如 EAI_NONAME, EAI_AGAIN 等）
         * @return 指向静态分配的错误描述字符串的指针（无需释放），例如：
         *         - EAI_NONAME → "Name or service not known"
         *         - EAI_AGAIN  → "Temporary failure in name resolution"
         * @note 该函数是线程安全的（MT-Safe）。注意不能用标准的 strerror() 来解析 EAI_* 错误码，
         *       因为它们不属于 errno 体系，而是 getaddrinfo 专有的错误编码空间。
         * @see getaddrinfo
         */
        LOG_ERROR << "DNS 解析失败: " << hostname << " 原因: " << gai_strerror(status);
        return false;
    }

    for (p = res; p != nullptr; p = p->ai_next) {
        struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
        /**
         * @brief 将网络字节序的 IPv4 二进制地址（struct in_addr）转换为点分十进制字符串
         * @signature char *inet_ntoa(struct in_addr in)
         * @param in 包含网络字节序 IPv4 地址的 in_addr 结构体（从 sockaddr_in.sin_addr 中获取）
         * @return 指向静态分配的缓冲区的指针，包含点分十进制格式的 IP 字符串（如 "117.72.11.81"）。
         *         该缓冲区为函数内部静态存储，后续调用 inet_ntoa() 会覆盖之前的结果
         * @note 此函数已被标记为 [[deprecated]]（POSIX 废弃），原因是：
         *       1. 返回值指向静态缓冲区，非线程安全（多线程下可能产生数据竞争）
         *       2. 仅支持 IPv4，不支持 IPv6
         *       推荐使用 inet_ntop() 替代，它接受用户提供的缓冲区且支持 IPv4/IPv6 双栈。
         *       此处因返回值立即赋值给 std::string（发生拷贝），线程安全问题在单线程 resolve 中不会体现。
         * @see inet_ntop, inet_pton
         */
        OutIp = inet_ntoa(addr->sin_addr);
        break;
    }

    /**
     * @brief 释放 getaddrinfo() 内部动态分配的 addrinfo 结果链表
     * @signature void freeaddrinfo(struct addrinfo *res)
     * @param res 指向 getaddrinfo() 返回的链表头指针。该函数会递归释放链表中所有节点
     *            （包括每个节点中的 ai_addr 和 ai_canonname 等动态分配的字段）
     * @note 每次成功调用 getaddrinfo() 后，必须且仅需调用一次 freeaddrinfo() 释放内存。
     *       不调用将导致内存泄漏；对同一指针重复调用将导致 double-free（未定义行为）。
     *       释放后不应再访问链表中的任何节点或字段。
     * @see getaddrinfo
     */
    freeaddrinfo(res);
    return true;
}

} // namespace MyServer