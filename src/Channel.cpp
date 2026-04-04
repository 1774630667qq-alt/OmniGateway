/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-19 15:20:34
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-22 22:14:23
 * @FilePath: /ServerPractice/src/Channel.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "Channel.hpp"
#include "EventLoop.hpp"
#include <sys/epoll.h>
#include <unistd.h> // 提供 close 函数

namespace MyServer {
    Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd), events_(0), revents_(0) {}

    void Channel::handleEvent() {
        if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
            // 连接被挂断了，并且没有可读事件了，说明是对方关闭了连接
            if (closeCallback_) {
                closeCallback_();
            }
        } 

        if (revents_ & EPOLLERR) {
            // 发生错误了
            if (closeCallback_) {
                closeCallback_();
            }
        }

        if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
            if (readCallback_) {
                readCallback_();
            }
        }

        if (revents_ & EPOLLOUT) {
            if (writeCallback_) {
                writeCallback_();
            }
        }
    }

    void Channel::enableWriting() {
        events_ |= EPOLLOUT;
        loop_->updateChannel(this);
    }

    void Channel::disableWriting() {
        events_ &= ~EPOLLOUT;
        loop_->updateChannel(this);
    }

    void Channel::enableReading() {
        events_ |= EPOLLIN | EPOLLET; // 添加边缘触发(ET)标志
        loop_->updateChannel(this);
    }

    void Channel::disableAll() {
        events_ = 0;
        loop_->updateChannel(this);
    }
}