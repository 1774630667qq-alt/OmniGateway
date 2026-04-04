#pragma once
#include <vector>
#include <functional>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstddef>

namespace MyServer {
    class ThreadPool {
        private:
        std::vector<std::thread> workers; // 工作线程池
        std::queue<std::function<void()>> tasks; // 任务队列
        std::mutex queue_mutex; // 保护任务队列的互斥锁
        /**
         * @brief std::condition_variable 是 C++11 引入的条件变量，用于多线程间的同步。
         * 
         * 它允许一个或多个线程在某些条件满足之前挂起（阻塞），直到另一个线程通知它们条件已经满足。
         * 
         * 常用接口介绍：
         * - wait(std::unique_lock<std::mutex>& lock): 
         *   阻塞当前线程，直到被 notify_one 或 notify_all 唤醒。调用时会自动释放 lock，被唤醒时会自动重新获取 lock。
         * - wait(std::unique_lock<std::mutex>& lock, Predicate pred): 
         *   阻塞当前线程，直到被唤醒且谓词 pred 返回 true。常用于防止虚假唤醒（spurious wakeup）。
         * - notify_one(): 
         *   随机唤醒一个等待在这个条件变量上的线程。如果没有线程等待，则该调用不产生任何影响。
         * - notify_all(): 
         *   唤醒所有等待在这个条件变量上的线程。
         */
        std::condition_variable condition; // 任务到来通知工作线程的条件变量
        bool stop; // 线程池停止标志

        public:
        ThreadPool(size_t threads): stop(false) {
            for (size_t i = 0; i < threads; ++i) {
                workers.emplace_back([this] {
                    while (true) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex);
                            this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                            if (this->stop && this->tasks.empty()) return; // 线程池停止且没有任务了，退出线程
                            task = std::move(this->tasks.front());
                            this->tasks.pop();
                        }
                        task(); // 执行任务
                    }
                });
            }
        }

        /**
         * @brief 向线程池中添加新的异步任务
         * @param task 需要在工作线程中执行的任务 (通常为 Lambda 表达式或 std::bind 绑定的函数)
         */
        void enqueue(std::function<void()> task) {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                tasks.emplace(std::move(task));
            }
            condition.notify_one(); // 通知一个工作线程有新任务了
        }

        ~ThreadPool() {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                stop = true; // 设置停止标志
            }
            condition.notify_all(); // 唤醒所有线程，让它们退出
            for (std::thread &worker : workers) {
                if (worker.joinable()) worker.join(); // 等待所有线程结束
            }
        }
    };
}