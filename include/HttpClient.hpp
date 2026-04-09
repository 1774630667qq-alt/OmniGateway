/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-08 15:56:09
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-08 17:44:19
 * @FilePath: /OmniGateway/include/HttpClient.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "EventLoop.hpp"
#include "TcpConnection.hpp"
#include "Connector.hpp"

namespace MyServer {
struct ApiConfig {
    std::string host;       // 例如 "api.edgefn.net"
    std::string path;       // 例如 "/v1/chat/completions"
    std::string apiKey;     // "sk-..."
    std::string targetModel;// "GLM-5"
};

/**
 * @class HttpClient
 * @brief 反向代理的客户端枢纽：负责与后端大模型 API 建立连接、发送请求、并解析流式响应
 */
class HttpClient : public std::enable_shared_from_this<HttpClient> {
public:
    // 回调定义：当从后端 API 拿到完整响应或一段 SSE 流时触发
    using HttpResponseCallback = std::function<void(const std::string& responseData)>;

    HttpClient(EventLoop* loop, const InetAddress& serverAddr, const ApiConfig& apiConfig);
    ~HttpClient();

    /**
     * @brief 启动客户端，非阻塞连接后端 API
     */
    void connect();

    /**
     * @brief 连接建立后，主动发送 HTTP 请求报文
     * @param requestStr 已经打包好的、发往 OpenAI 协议格式的 HTTP 报文
     */
    void sendRequest(const std::string& requestStr);

    void setResponseCallback(HttpResponseCallback cb) {
        responseCallback_ = std::move(cb);
    }

private:
    // --- 底层网络事件回调 ---
    void onConnection(int sockfd);  // Connector 成功后触发
    void onMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf); // 后端 API 来数据了
    void onClose(const std::shared_ptr<TcpConnection>& conn);

    EventLoop* loop_;                                // HttpClient 运行的独立事件循环
    std::shared_ptr<Connector> connector_;           // 负责底层非阻塞修路
    std::shared_ptr<TcpConnection> backendConn_;     // 负责在修好的路上收发数据
    ApiConfig apiConfig_;                            // 后端 API 配置
    
    HttpResponseCallback responseCallback_;          // 跨线程桥梁：通过这个回调通知前端 Server
    
    bool connected_;                                 // 连接状态
};
} // namespace MyServer