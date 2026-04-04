/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-27 17:15:20
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-27 17:55:08
 * @FilePath: /ServerPractice/src/AsyncLogging.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "AsyncLogging.hpp"
#include <iostream>
#include <chrono>

namespace MyServer {

    AsyncLogging::AsyncLogging(const std::string& basename)
        : basename_(basename),
          running_(false),
          currentBuffer_(new Buffer),
          nextBuffer_(new Buffer) {
        currentBuffer_->bzero();
        nextBuffer_->bzero();
        buffers_.reserve(16); // 预分配一点空间
    }

    AsyncLogging::~AsyncLogging() {
        if (running_) {
            stop();
        }
    }

    void AsyncLogging::append(const char* logline, int len) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (currentBuffer_->avail() >= len) {
                currentBuffer_->append(logline, len);
            } else {
                buffers_.push_back(std::move(currentBuffer_));
                if (nextBuffer_) {
                    currentBuffer_ = std::move(nextBuffer_);
                } else {
                    currentBuffer_.reset(new Buffer);
                }
                currentBuffer_->append(logline, len);
                cond_.notify_one();
            }
        }
    }

    void AsyncLogging::threadFunc() {
        // 1. 在后台线程里，准备两个新的“空桶”。这两个空桶专门用来和前台交换！
        BufferPtr newBuffer1(new Buffer);
        BufferPtr newBuffer2(new Buffer);
        newBuffer1->bzero();
        newBuffer2->bzero();

        // 2. 准备一个后台专用的队列，用来接收前台装满的桶
        BufferVector buffersToWrite;
        buffersToWrite.reserve(16);

        while (running_) {
            {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cond_.wait_for(lock, std::chrono::seconds(3), [this] { return !buffers_.empty() || !running_; });
                    buffers_.push_back(std::move(currentBuffer_));
                    currentBuffer_ = std::move(newBuffer1);
                    buffersToWrite.swap(buffers_);
                    if (!nextBuffer_) {
                        nextBuffer_ = std::move(newBuffer2);
                    }
                }
            }
            // 9. 防御性编程：如果瞬间涌入海量日志（比如 buffersToWrite 超过了 25 个），说明磁盘完全跟不上了。
            // 工业级做法：直接砍掉多余的日志，只保留前两个（或者直接清空过多的部分），保命要紧！
            if (buffersToWrite.size() > 25) {
                buffersToWrite.erase(buffersToWrite.begin() + 2, buffersToWrite.end());
            }

            // 10. 遍历 buffersToWrite 里的所有满桶，把数据打印出来！
            // (暂时不写文件操作，我们直接用 std::cout.write 打印到屏幕上来验证)
            for (const auto& buffer : buffersToWrite) {
                std::cout.write(buffer->data(), buffer->length());
            }
            std::cout.flush(); // 强制刷新输出流

            // 11. 重新回收备用桶！
            if (!newBuffer1) {
                newBuffer1 = std::move(buffersToWrite.back());
                buffersToWrite.pop_back();
                newBuffer1->reset();
            }
            if (!newBuffer2) {
                newBuffer2 = std::move(buffersToWrite.back());
                buffersToWrite.pop_back();
                newBuffer2->reset();
            }

            // 12. 清空 buffersToWrite，开启下一轮死循环！
            buffersToWrite.clear();
        }
        std::cout.flush();
    }
} // namespace MyServer