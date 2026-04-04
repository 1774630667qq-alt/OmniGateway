/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-19 16:39:02
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-31 16:08:09
 * @FilePath: /ServerPractice/src/Accept.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Logger.hpp"
#include "Accept.hpp"
#include <sys/socket.h> // 提供 socket 函数及数据结构
#include <netinet/in.h> // 提供 IPv4 的 sockaddr_in 结构体
#include <arpa/inet.h>  // 提供 IP 地址转换函数
#include <unistd.h>     // 提供 close 函数
#include <cstring>      // 提供 memset
#include <fcntl.h>      // 提供 fcntl

namespace MyServer {
    Acceptor::Acceptor(EventLoop* loop, int port) : loop_(loop), listen_fd_(-1), acceptChannel_(nullptr), port_(port) {
        // --- 1. 创建监听套接字 ---
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == -1) {
            LOG_FATAL << "Socket 创建失败!";
        }
        LOG_INFO << "Socket 创建成功，fd: " << listen_fd_;

        // 开启端口复用
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // 绑定地址和端口
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listen_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            LOG_FATAL << "Bind 失败!";
            close(listen_fd_);
        }

        // 设置监听套接字为非阻塞模式
        fcntl(listen_fd_, F_SETFL, fcntl(listen_fd_, F_GETFL, 0) | O_NONBLOCK);

        acceptChannel_ = new Channel(loop_, listen_fd_);
        acceptChannel_->setReadCallback(std::bind(&Acceptor::handleRead, this));
    }

    Acceptor::~Acceptor() {
        acceptChannel_->disableAll();
        if (listen_fd_ != -1) {
            close(listen_fd_);
        }
        delete acceptChannel_;
    }

    void Acceptor::listen() {
        if (::listen(listen_fd_, 128) == -1) {
            LOG_FATAL << "Listen 失败!";
            close(listen_fd_);
        }
        LOG_INFO << "服务器启动，正在监听 " << port_ << " 端口...";
        acceptChannel_->enableReading();
    }

    void Acceptor::handleRead() {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        LOG_INFO << "有新连接到来，正在接受...";
        while (true) {
            int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 没有更多的连接了，退出循环
                    break;
                } else {
                    LOG_ERROR << "Accept 失败!";
                    break;
                }
            }

            // 设置新接入的客户端套接字为非阻塞模式
            fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            LOG_INFO << "新客户端连接: " << ip_str << ":" << ntohs(client_addr.sin_port) << ", fd: " << client_fd;

            if (newConnectionCallback_) {
                newConnectionCallback_(client_fd);
            }
        }
    }
}