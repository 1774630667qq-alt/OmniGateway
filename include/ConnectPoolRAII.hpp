#pragma once
#include "ConnectionPool.hpp"

namespace MyServer {
    /**
     * @brief 连接池 RAII 守卫：构造时借出连接，析构时自动归还
     * @details
     *   [异步生命周期适配]
     *   由于 HttpClient 的 connect() 是异步操作（DNS → TCP → TLS → SSE），
     *   栈上 RAII 会在回调函数 return 时过早析构。
     *   正确用法是将本守卫包装为 shared_ptr 并捕获到 responseCallback_ 的 lambda 中：
     *
     *   auto connGuard = std::make_shared<ConnectionPoolRAII>(&client, pool, ioLoop);
     *   client->setResponseCallback([connGuard](...) { ... });
     *
     *   当 onClose 触发 → std::move(responseCallback_) → lambda 析构
     *   → connGuard 引用计数归零 → RAII 析构 → freeConn() 自动归还
     */
    class ConnectionPoolRAII {
    public:
        /**
         * @brief 构造：从连接池中借出一个 HttpClient
         * @param conn 输出参数，借出的 HttpClient 智能指针写入此处
         * @param pool 连接池实例指针
         * @param iloop 当前请求所在的 EventLoop（按桶借出）
         */
        ConnectionPoolRAII(std::shared_ptr<HttpClient>* conn, ConnectionPool* pool, EventLoop* iloop) {
            *conn = pool->getConn(iloop);
            conn_ = *conn;
            pool_ = pool;
            loop_ = iloop;
        };

        /**
         * @brief 析构：自动调用 freeConn() 将连接归还到池中
         */
        ~ConnectionPoolRAII() {
            if (conn_ && pool_) {
                pool_->freeConn(loop_, conn_);
            }
        }
    private:
        std::shared_ptr<HttpClient> conn_;  ///< 借出的 HttpClient 引用
        ConnectionPool* pool_;              ///< 连接池实例（非拥有）
        EventLoop* loop_;                   ///< 连接所属的 EventLoop 分桶
    };
} // namespace MyServer