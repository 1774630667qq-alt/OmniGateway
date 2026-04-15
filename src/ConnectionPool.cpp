/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-15 12:08:42
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-15 19:20:00
 * @FilePath: /OmniGateway/src/ConnectionPool.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "ConnectionPool.hpp"
#include "Logger.hpp"

namespace MyServer {
    void ConnectionPool::Init(EventLoopThreadPool* loopPool, ThreadPool* pool, std::string hostname, 
        int port, ApiConfig& apiConfig) {
        std::lock_guard<std::mutex> lock_guard(mutex_);
        
        if (isInit_) {
            LOG_WARNING << "重复初始化，忽略本次调用";
            return;            
        }

        hostname_ = hostname;
        port_ = port;
        apiConfig_ = apiConfig;
        pool_ = pool;
        isInit_ = true;

        // 懒创建：仅初始化分桶结构，不预建连接
        // 连接在首次 getConn() 时按需创建，避免启动时大量空闲连接被后端超时断开
        int Size = loopPool->size();
        for (int i = 0; i < Size; ++i) {
            EventLoop* loop = loopPool->getNextLoop();
            useCountMap_[loop] = 0;
            poolMap_[loop] = {};
        }

        LOG_INFO << "连接池初始化完成（懒创建模式），分桶数: " << Size;
    }

    void ConnectionPool::ClosePool() {
        std::lock_guard<std::mutex> lock(mutex_);
        isClosed_ = true;

        for (auto& [loop, v] : poolMap_) {
            v.clear();
        }
        poolMap_.clear();
        useCountMap_.clear();

        LOG_INFO << "连接池已关闭，所有连接已销毁";
    }

    std::shared_ptr<HttpClient> ConnectionPool::getConn(EventLoop* iloop) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (isClosed_) {
            LOG_ERROR << "连接池已关闭";
            return nullptr;
        }

        auto& pool = poolMap_[iloop];
        
        // LIFO 策略：尾部是最近归还的连接（最新鲜）
        // 如果尾部都已过期，比它更早入栈的连接必然全部过期，直接清空
        if (!pool.empty()) {
            auto conn = pool.back();
            if (conn->isConnected()) {
                pool.pop_back();
                LOG_INFO << "从池中复用存活连接（池中剩余: " << pool.size() << "）";
                useCountMap_[iloop]++;
                return conn;
            }
            // 尾部已过期 → 全桶作废，一次性清空
            LOG_INFO << "最新连接已过期，清空整桶（丢弃 " << pool.size() << " 个）";
            pool.clear();
        }

        // 池中无可用连接，按需创建新的 HttpClient（不预连接，由调用者触发 connect）
        LOG_INFO << "池中无可用连接，按需创建新 HttpClient";
        auto conn = std::make_shared<HttpClient>(iloop, pool_, hostname_, port_, apiConfig_);
        useCountMap_[iloop]++;
        return conn;
    }

    void ConnectionPool::freeConn(EventLoop* iloop, std::shared_ptr<HttpClient> conn) {
        if (!conn) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        conn->reset();

        // 如果连接池已关闭，丢弃连接
        if (isClosed_) {
            return;
        }

        useCountMap_[iloop]--;

        if (conn->isConnected()) {
            // 连接仍存活，放回尾部以便下次 LIFO 复用
            poolMap_[iloop].push_back(conn);
            LOG_INFO << "连接归还至池中（池中数量: " << poolMap_[iloop].size() << "）";
        } else {
            LOG_INFO << "归还时连接已断开，丢弃";
        }
    }
} // namespace MyServer