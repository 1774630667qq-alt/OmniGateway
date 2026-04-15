/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-15 12:08:42
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-15 19:20:00
 * @FilePath: /OmniGateway/include/ConnectionPool.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include "HttpClient.hpp"
#include "EventLoop.hpp"
#include "EventLoopThreadPool.hpp"
#include "ThreadPool.hpp"
#include <unordered_map>
#include <mutex>
#include <vector>

namespace MyServer {

    /**
     * @brief HttpClient 连接池（单例模式）
     * @details 按 EventLoop 分桶管理 HttpClient 实例，采用懒创建 + LIFO 复用策略。
     *   [懒创建] 启动时不预热连接，请求到来时按需创建，避免大量空闲连接被后端超时断开。
     *   [LIFO 复用] 最近归还的连接最先被复用，提高 keep-alive 命中率。
     *   [空闲超时] 借出时自动丢弃超过 HttpClient::kMaxIdleSeconds 的过期连接。
     */
    class ConnectionPool {
    private:

        std::unordered_map<EventLoop* , int> useCountMap_;      ///< 已借出的连接数量（调试用）
        bool isClosed_;                                         ///< 连接池是否关闭标志位
        bool isInit_;                                           ///< 连接池是否初始化标志位
        ThreadPool* pool_;                                      ///< 用于 DNS 解析的线程池
        std::string hostname_;                                  ///< 后端 API 域名（用于动态创建连接）
        int port_;                                              ///< 后端 API 端口
        ApiConfig apiConfig_;                                   ///< 后端 API 配置（域名、路径、密钥、模型）
        std::unordered_map<EventLoop* , std::vector<std::shared_ptr<HttpClient>>> poolMap_;  ///< 连接池（LIFO：最近归还的连接最先被复用）
        std::mutex mutex_;                                      ///< 互斥锁

        ConnectionPool(): isClosed_(false), isInit_(false), pool_(nullptr) {}
        
        ~ConnectionPool() { ClosePool(); }

        // 单例模式禁用拷贝构造和赋值构造
        ConnectionPool(const ConnectionPool&) = delete;
        ConnectionPool& operator=(const ConnectionPool&) = delete;

    public:
        /**
         * @brief 获取全局唯一的 https 连接池
         * 
         * @return ConnectionPool& 返回连接池的单例引用
         */
        static ConnectionPool& instance() {
            static ConnectionPool instance;
            return instance;
        }

        /**
         * @brief 关闭连接池，并销毁所有连接
         * 
         */
        void ClosePool();

        /**
         * @brief 初始化单例连接池（懒创建模式，不预建连接）
         * 
         * @param loopPool 传入的EventLoop线程池
         * @param pool 用以DNS解析的线程池
         * @param hostname 大模型API的域名
         * @param port 大模型API的端口
         * @param apiConfig 大模型API的配置
         */
        void Init(EventLoopThreadPool* loopPool, ThreadPool* pool, std::string hostname, 
            int port, ApiConfig& apiConfig);

        /**
        * @brief 从连接池中借出一个 HttpClient（优先复用存活连接）
        * 
        * @param iloop 当前请求所在的 EventLoop
        * @return std::shared_ptr<HttpClient> 借出的 HttpClient 智能指针
        * @details
        *   [策略] LIFO 遍历：从最近归还的连接开始查找，优先复用存活连接。
        *   若池中无存活连接，按需创建新的 HttpClient（不预连接，由调用者触发 connect）。
        */
        std::shared_ptr<HttpClient> getConn(EventLoop* iloop);

        /**
         * @brief 归还连接到池中
         * @param iloop 连接所属的 EventLoop
         * @param conn 待归还的 HttpClient 智能指针
         * @details 内部调用 conn->reset() 清理请求状态。
         *   若连接仍存活且池未满，推回池尾复用；否则丢弃。线程安全。
         */
        void freeConn(EventLoop* iloop, std::shared_ptr<HttpClient> conn);
    };
} 