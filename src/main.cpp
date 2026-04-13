/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-04 16:15:45
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-13 20:56:47
 * @FilePath: /OmniGateway/src/main.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "EventLoop.hpp"
#include "HttpServer.hpp"
#include "HttpClient.hpp"
#include "ProtocolTranslator.hpp"
#include "SSLManager.hpp"
#include "Logger.hpp"
#include <signal.h>
#include <memory>

using namespace MyServer;

int main() {
    // 1. 基础环境初始化 (忽略管线破裂信号，防止强退)
    signal(SIGPIPE, SIG_IGN);
    MyServer::initGlobalLogger("OmniGatewayLog");
    SSLManager::init();

    EventLoop loop;
    ThreadPool pool(4); // 供 HttpServer 的回调使用

    // 2. 启动前端接待服务器 (监听 8080 端口，接收 Claude Code 的请求)
    HttpServer gateway(&loop, 8080, &pool);
    gateway.setThreadNum(4);

    // 3. 配置后端大模型 API (此处为白山智算)
    ApiConfig backendConfig;
    backendConfig.host = "api.edgefn.net";
    backendConfig.path = "/v1/chat/completions";
    // 从环境变量读取密钥，避免硬编码泄露
    const char* envKey = std::getenv("OMNI_API_KEY");
    backendConfig.apiKey = envKey ? envKey : "sk-your-api-key-here"; // 【务必修改】设置环境变量 OMNI_API_KEY
    backendConfig.targetModel = "GLM-5";

    // 4. 网关核心大闭环：接管前端的 TCP 连接 (frontConn)
    gateway.setHttpCallback([&loop, backendConfig](const HttpRequest& req, const std::shared_ptr<TcpConnection>& frontConn) {
        LOG_INFO << "收到前端请求: " << req.getPath();

        // 提取纯路径（去掉查询参数 ?xxx）
        std::string purePath = req.getPath();
        size_t queryPos = purePath.find('?');
        if (queryPos != std::string::npos) {
            purePath = purePath.substr(0, queryPos);
        }

        // 拦截 Claude Code 发往 Anthropic 官方的请求
        if (purePath == "/v1/messages" && req.getMethod() == "POST") {
            
            // 【步骤 A】：立刻向 Claude Code 发送 HTTP 响应头，宣告开启流式通道！
            // 注意：frontConn 属于 IO 子线程，必须投递到其 EventLoop
            std::string sseHeaders = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n\r\n";
            frontConn->getLoop()->queueInLoop([frontConn, sseHeaders]() {
                frontConn->send(sseHeaders);
            });

            // 【步骤 B】：同声传译 (Claude 格式 -> OpenAI 格式)
            std::string openaiReq = ProtocolTranslator::translateRequest(req.getBody(), backendConfig.targetModel);
            if (openaiReq.empty()) {
                LOG_ERROR << "请求翻译失败，断开前端连接";
                frontConn->getLoop()->queueInLoop([frontConn]() {
                    frontConn->forceClose();
                });
                return;
            }

            // 【步骤 C + D】：在 main loop 线程上创建 HttpClient 并发起连接
            // HttpClient::connect() 会修改 main loop 的 epoll，必须在 main loop 线程执行！
            // 否则多个 pool 线程同时 connect() 会发生竞态条件导致段错误。
            loop.queueInLoop([&loop, frontConn, backendConfig, openaiReq]() {
                InetAddress serverAddr(443, "198.18.0.87");
                auto client = std::make_shared<HttpClient>(&loop, serverAddr, backendConfig);
                client->setRequestBody(openaiReq);

                auto streamState = std::make_shared<StreamState>();
                client->setResponseCallback([frontConn, client, streamState](const std::string& openaiSse) {
                    // 同声传译 (OpenAI 数据流 -> Claude 数据流)
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

                LOG_INFO << "请求翻译完毕，代理客户端发起后台连接...";
                client->connect();
            });
        } 
        // Claude Code 启动时的健康检查端点
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
        // Claude Code 可能发送的其他 API 探测请求，返回 200 避免误判
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
        else {
            // 非 API 路径的兜底路由（不 forceClose，避免多线程堆损坏）
            std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            frontConn->send(notFound);
            LOG_INFO << "未知路径 (404): " << req.getPath();
        }
    });

    LOG_INFO << "🚀 OmniGateway 引擎启动成功！正在监听 8080 端口...";
    gateway.start();
    loop.loop();
    
    SSLManager::destroy();
    return 0;
}