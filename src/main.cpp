/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-04 16:15:45
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-14 11:34:00
 * @FilePath: /OmniGateway/src/main.cpp
 * @Description: OmniGateway 主入口：协议翻译网关，将 Claude Code 请求转发至 OpenAI 兼容后端
 */
#include "EventLoop.hpp"
#include "HttpServer.hpp"
#include "HttpClient.hpp"
#include "ProtocolTranslator.hpp"
#include "ConfigManager.hpp"
#include "SSLManager.hpp"
#include "ConnectionPool.hpp"
#include "ConnectPoolRAII.hpp"
#include "Logger.hpp"
#include <signal.h>
#include <memory>

using namespace MyServer;

int main() {
    // ================================================================
    // 1. 基础环境初始化
    // ================================================================
    signal(SIGPIPE, SIG_IGN);
    MyServer::initGlobalLogger("OmniGatewayLog");
    SSLManager::init();

    // ================================================================
    // 2. 加载配置文件
    // ================================================================
    auto& config = ConfigManager::getInstance();
    if (!config.loadConfig("../gateway_config.json")) {
        LOG_ERROR << "配置文件加载失败，程序退出！";
        return 1;
    }
    const auto& serverCfg = config.getServerConfig();
    auto apiCfg = config.getApiConfig();

    // ================================================================
    // 3. 启动事件循环与线程池
    // ================================================================
    EventLoop loop;
    ThreadPool pool(4); // 通用线程池：DNS 解析等阻塞操作在此执行

    // ================================================================
    // 4. 启动前端 HTTP 网关服务器
    // ================================================================
    HttpServer gateway(&loop, serverCfg.port, &pool);
    gateway.setThreadNum(serverCfg.threadNum);

    // ================================================================
    // 6. 网关核心路由：接管前端 TCP 连接并转发至后端大模型
    // ================================================================
    gateway.setHttpCallback([&pool, apiCfg](const HttpRequest& req, const std::shared_ptr<TcpConnection>& frontConn) mutable {
        LOG_INFO << "收到前端请求: " << req.getPath();

        // 提取纯路径（去掉查询参数 ?xxx）
        std::string purePath = req.getPath();
        size_t queryPos = purePath.find('?');
        if (queryPos != std::string::npos) {
            purePath = purePath.substr(0, queryPos);
        }

        // ============================================================
        // 路由 1: /v1/messages (POST) — 核心代理路由
        // ============================================================
        if (purePath == "/v1/messages" && req.getMethod() == "POST") {
            
            // 【步骤 A】：立刻发送 SSE 响应头，开启流式通道
            std::string sseHeaders = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n\r\n";
            frontConn->getLoop()->queueInLoop([frontConn, sseHeaders]() {
                frontConn->send(sseHeaders);
            });

            // 【步骤 B】：同声传译 (Claude 格式 -> OpenAI 格式)
            std::string openaiReq = ProtocolTranslator::translateRequest(req.getBody(), apiCfg.targetModel);
            if (openaiReq.empty()) {
                LOG_ERROR << "请求翻译失败，断开前端连接";
                frontConn->getLoop()->queueInLoop([frontConn]() {
                    frontConn->forceClose();
                });
                return;
            }

            // 【步骤 C】：从连接池借出 HttpClient（长连接可能已预热）
            // connGuard 被 responseCallback 和 responseCompleteCallback 同时捕获，
            // 当 reset() 清空两个回调后，引用计数归零 → RAII 析构 → freeConn()
            EventLoop* ioLoop = frontConn->getLoop();
            std::shared_ptr<HttpClient> client;
            auto connGuard = std::make_shared<ConnectionPoolRAII>(&client, &ConnectionPool::instance(), ioLoop);
            
            if (!client) {
                LOG_ERROR << "连接池借出失败";
                frontConn->getLoop()->queueInLoop([frontConn]() {
                    frontConn->forceClose();
                });
                return;
            }

            client->setRequestBody(openaiReq);

            // 【步骤 D】：注册响应回调 — 同声传译 (OpenAI 数据流 -> Claude 数据流)
            // 注意：此 lambda 不捕获 connGuard，避免与 responseCompleteCallback 形成引用计数死锁
            auto streamState = std::make_shared<StreamState>();
            client->setResponseCallback([frontConn, streamState](const std::string& openaiSse) {
                std::string claudeSse = ProtocolTranslator::translateSseEvent(openaiSse, *streamState);
                if (!claudeSse.empty()) {
                    std::string data = claudeSse;
                    frontConn->getLoop()->queueInLoop([frontConn, data]() {
                        frontConn->send(data);
                    });
                }

                // 检测流结束：后端发来 [DONE] 时关闭前端连接
                std::string trimmed = openaiSse;
                while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\n' || trimmed.front() == '\r')) trimmed.erase(trimmed.begin());
                while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\n' || trimmed.back() == '\r')) trimmed.pop_back();
                if (trimmed == "[DONE]" || trimmed == "data: [DONE]") {
                    LOG_INFO << "SSE 流结束，投递关闭前端连接";
                    frontConn->getLoop()->queueInLoop([frontConn]() {
                        frontConn->forceClose();
                    });
                }
            });

            // 【步骤 E】：注册响应完成回调 — 长连接归还触发点
            // 当 chunked 尾块到达且后端支持 keep-alive 时，HttpClient 调用此回调。
            // reset() 清空此 lambda → connGuard 引用减少 → 配合 responseCallback 清空后
            // 引用计数归零 → RAII 析构 → freeConn() → 连接归还池中（仍存活）
            client->setResponseCompleteCallback([connGuard]() {
                // connGuard 捕获在此，生命周期延长至 reset() 时
            });

            LOG_INFO << "请求翻译完毕，从连接池借出客户端发起后台连接...";
            client->connect();
        } 
        // ============================================================
        // 路由 2: /api/hello — Claude Code 健康检查
        // ============================================================
        else if (purePath == "/api/hello") {
            std::string body = "{\"status\":\"ok\"}";
            std::string resp = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            frontConn->send(resp);
            LOG_INFO << "健康检查通过: /api/hello";
        }
        // ============================================================
        // 路由 3: /v1/* 和 /api/* — 兜底路由，返回 200
        // ============================================================
        else if (purePath.find("/v1/") == 0 || purePath.find("/api/") == 0) {
            std::string body = "{}";
            std::string resp = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            frontConn->send(resp);
            LOG_INFO << "API 兜底路由 (200): " << req.getPath();
        }
        // ============================================================
        // 路由 4: 其他路径 — 404
        // ============================================================
        else {
            std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            frontConn->send(notFound);
            LOG_INFO << "未知路径 (404): " << req.getPath();
        }
    });

    LOG_INFO << "🚀 OmniGateway 引擎启动成功！正在监听 " << serverCfg.port << " 端口...";
    gateway.start();

    // ================================================================
    // 5. 初始化 HTTPS 长连接池（必须在 IO 线程启动后）
    // ================================================================
    ConnectionPool::instance().Init(
        gateway.getThreadPool(), &pool, apiCfg.host, 443, apiCfg);

    loop.loop();
    
    SSLManager::destroy();
    return 0;
}