/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-08 15:56:09
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-15 13:31:09
 * @FilePath: /OmniGateway/include/HttpClient.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include "EventLoop.hpp"
#include "TcpConnection.hpp"
#include "Connector.hpp"
#include "HttpParser.hpp"
#include "ConfigManager.hpp"
#include <chrono>

namespace MyServer {
class ThreadPool;

/**
 * @class HttpClient
 * @brief 反向代理的客户端枢纽：负责与后端大模型 API 建立连接、发送请求、并解析流式响应
 * @details
 *   [架构角色]
 *   在 OmniGateway 的反向代理架构中，HttpClient 处于"下游客户端"的角色。
 *   当前端用户通过 HttpServer 发来请求后，业务层从连接池中借出一个 HttpClient 实例，
 *   由它主动连接后端大模型 API 节点（如智谱、OpenAI），转发请求并将响应回传。
 * 
 *   [生命周期]
 *   1. 构造阶段：接收 EventLoop、目标地址和 API 配置，内部创建 Connector 但不立即发起连接。
 *   2. 准备阶段：外部调用 setRequestBody() 存入待发送的 JSON，setResponseCallback() 注册回调。
 *   3. 连接阶段：调用 connect() 后，Connector 在 IO 线程中以非阻塞方式发起 TCP 三次握手。
 *   4. TLS 握手：TCP 连通后，由 TcpConnection::doTlsHandshake() 完成 SSL 加密通道建立。
 *   5. 自动发送：握手成功触发 connectionCallback_，内部自动调用 sendRequest() 发出 HTTP 报文。
 *   6. 数据交互：onMessage() 接收并解析后端响应，通过 responseCallback_ 回传给前端。
 *   7. 销毁阶段：连接断开或出错时，onClose() 被触发，清理 TcpConnection 资源。
 * 
 *   [线程模型]
 *   HttpClient 绑定到指定的 EventLoop 线程上运行。所有的网络事件回调
 *   （onConnection, onMessage, onClose）都在该 loop 所在的 IO 线程中被调度执行。
 *   通过 responseCallback_ 将结果跨线程传递给前端 HttpServer。
 */
class HttpClient : public std::enable_shared_from_this<HttpClient> {
public:
    /// 响应回调类型：当从后端 API 收到完整 HTTP 响应或一段 SSE 流式数据时触发
    using HttpResponseCallback = std::function<void(const std::string& responseData)>;

    /// 响应完成回调类型：SSE 流正常结束时触发（区别于 onClose 的异常断开）
    /// 连接池 RAII 守卫通过此回调得知请求已完成，自动归还连接
    using ResponseCompleteCallback = std::function<void()>;

    /**
     * @brief 构造函数：初始化客户端，绑定事件循环与连接器
     * @signature HttpClient(EventLoop* loop, ThreadPool* pool, const std::string& hostname, int port, ApiConfig& apiConfig);
     * @param loop 当前 HttpClient 将要运行在的 EventLoop（IO 线程）
     * @param pool 通用线程池，用于异步 DNS 解析
     * @param hostname 后端 API 域名（如 "api.edgefn.net"）
     * @param port 后端 API 端口（如 443）
     * @param apiConfig 后端 API 的配置信息（域名、路径、密钥、模型名）
     * @details
     *   构造阶段仅保存配置信息，不会立即发起网络连接。
     *   必须显式调用 connect() 才会启动连接状态机。
     */
    HttpClient(EventLoop* loop, ThreadPool* pool, const std::string& hostname, int port, ApiConfig& apiConfig);

    /**
     * @brief 析构函数：释放客户端资源并执行安全清理
     */
    ~HttpClient();

    /**
     * @brief 启动客户端，非阻塞连接后端 API
     * @signature void connect();
     */
    void connect();

    /**
     * @brief 存入待发送的 JSON 请求体
     * @signature void setRequestBody(const std::string& body);
     * @param body 已经转换好的 OpenAI 格式的 JSON 字符串
     * @details
     *   [职责]
     *   将待发送数据暂存到 pendingRequestBody_ 中。
     *   外部调用 connect() 后，当 TCP + TLS 握手全部完成时，
     *   HttpClient 内部会自动调用 sendRequest() 将该数据组装为 HTTP 报文发出。
     * 
     *   [典型调用顺序]
     *   1. client->setRequestBody(jsonStr);  // 存入数据
     *   2. client->setResponseCallback(cb);  // 注册回调
     *   3. client->connect();                // 启动连接
     *   后续全自动：TCP 连接 → TLS 握手 → sendRequest → onMessage
     */
    void setRequestBody(const std::string& body) {
        pendingRequestBody_ = body;
    }

    /**
     * @brief 设置响应回调函数
     * @signature void setResponseCallback(HttpResponseCallback cb);
     * @param cb 当后端 API 返回数据时被触发的回调。
     *   该回调是 HttpClient 与上层业务（如 HttpServer）之间的跨线程桥梁，
     *   用于将后端响应数据传递回前端连接。
     */
    void setResponseCallback(HttpResponseCallback cb) {
        responseCallback_ = std::move(cb);
    }

    /**
     * @brief 清空上一次请求级状态（Body、回调、Buffer），保留连接级状态（backendConn_、connected_）
     * @details 长连接模式下，reset() 不销毁底层 TcpConnection，使连接可被复用。
     */
    void reset();

    /**
     * @brief 当前连接是否处于已连接且可复用的状态
     * @return true 底层 TcpConnection 存活且未超过空闲超时阈值
     * @details 空闲超时设为 kMaxIdleSeconds 秒（后端 API 通常在 ~60 秒后主动断开空闲连接），
     *   提前标记为不可用，避免在连接即将被关闭时发起请求。
     */
    bool isConnected() const {
        if (!connected_ || !backendConn_) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - lastActiveTime_).count();
        return elapsed < kMaxIdleSeconds;
    }

    /**
     * @brief 设置响应完成回调（由连接池 RAII 守卫注册）
     * @param cb SSE 流正常结束时触发的回调，用于归还连接到池中
     */
    void setResponseCompleteCallback(ResponseCompleteCallback cb) {
        responseCompleteCallback_ = std::move(cb);
    }

    // 禁用拷贝/赋值：HttpClient 持有活跃网络连接资源，
    // 连接池通过 shared_ptr 管理对象引用，不需要值语义。
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

private:
    /**
     * @brief Connector 连接成功后的交接回调
     * @signature void onConnection(int sockfd);
     * @param sockfd Connector 成功建立 TCP 连接后移交的已连通套接字文件描述符
     * @details
     *   [当前实现]
     *   1. 创建 SSL 对象并通过 SSL_set_fd() 绑定到 sockfd。
     *   2. 基于 sockfd 创建 TcpConnection（存入 backendConn_）。
     *   3. 注册 onMessage、onClose、connectionCallback_ 回调。
     *   4. 在 IO 线程中调用 connectEstablished() + doTlsHandshake()。
     */
    void onConnection(int sockfd);

    /**
     * @brief 后端 API 返回数据时的读事件回调
     * @signature void onMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);
     * @param conn 与后端 API 通信的 TcpConnection 智能指针
     * @param buf 读缓冲区指针，包含从后端接收到的原始 HTTP 响应数据
     * @details
     *   [当前实现]
     *   分三个阶段处理：
     *   1. 头部解析：委托 httpParser_ 解析状态行和头部，提取状态码和 Transfer-Encoding。
     *   2. 错误实体：若状态码非 200，收集完整错误 JSON 后通过 responseCallback_ 回传。
     *   3. Chunked + SSE：解析十六进制长度 → 提取原始数据 → 按 "\n\n" 切割 SSE 事件 → 回传前端。
     */
    void onMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);

    /**
     * @brief 后端连接关闭时的回调
     * @signature void onClose(const std::shared_ptr<TcpConnection>& conn);
     * @param conn 即将关闭的与后端 API 的 TcpConnection 智能指针
     * @details
     *   [当前实现]
     *   1. 更新 connected_ 状态，释放 backendConn_ 强引用。
     *   2. 异常熔断通知：若网络突然断开，通过 responseCallback_
     *      发送一个内部错误信号，防止前端死等。
     */
    void onClose(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 组装并发送完整的 HTTP POST 请求（内部方法）
     * @signature void sendRequest();
     * @details
     *   [职责]
     *   将 pendingRequestBody_ 中暂存的 JSON 包装成合法的 HTTP 报文
     *   （请求行 + 请求头 + Connection: keep-alive + 空行 + 请求体），
     *   通过 backendConn_->send() 发出。
     * 
     *   [调用时机]
     *   - 首次请求：由 connectionCallback_ 在 TLS 握手成功后自动触发
     *   - 复用请求：由 connect() 检测到已有连接后直接调用
     */
    void sendRequest();

    /**
     * @brief 域名解析完成后，建立 TCP 连接
     * @signature void doConnection(const InetAddress& resolvedAddr);
     * @param resolvedAddr 域名解析后的 IP 地址和端口
     */
    void doConnection(const InetAddress& resolvedAddr);

    /**
     * @brief HTTP 响应体解析状态机枚举
     * @details 头部解析阶段已委托给 HttpParser（响应模式），
     *   此枚举仅管理头部解析完成后的 Chunked 数据处理流程。
     */
    enum ParseState {
        kExpectChunkSize,  ///< 正在解析 Chunked 块的十六进制长度行
        kExpectChunkData,  ///< 正在读取真实的 Chunk 数据
        kExpectErrorBody   ///< 正在读取非 200 响应的错误 JSON 实体
    };

    /// 通用 HTTP 响应解析器（响应模式），负责解析状态行和头部
    HttpParser httpParser_{HttpParser::ParserMode::kResponseMode};
    ParseState parseState_ = kExpectChunkSize; ///< 当前体解析状态（头部由 httpParser_ 管理）
    size_t currentChunkSize_ = 0;              ///< 当前 Chunk 块剩余需要读取的字节数
    size_t expectedErrorLength_ = 0;           ///< 非 200 错误响应的 Content-Length
    Buffer chunkedBuffer_;                     ///< 专用缓冲区：存放剥离十六进制长度后的纯净数据

    EventLoop* loop_;                                ///< 所绑定的事件循环（IO 线程）
    std::shared_ptr<Connector> connector_;           ///< 主动连接器：负责非阻塞 TCP 三次握手
    std::shared_ptr<TcpConnection> backendConn_;     ///< 与后端 API 的活跃 TCP 连接（握手成功后创建）
    ApiConfig apiConfig_;                            ///< 后端 API 配置（域名、路径、密钥、模型）
    
    HttpResponseCallback responseCallback_;          ///< 跨线程桥梁：将后端响应回传至前端 HttpServer
    ResponseCompleteCallback responseCompleteCallback_; ///< 响应正常完成时归还连接的回调
    
    bool connected_ = false;                         ///< 当前是否与后端 API 处于已连接状态
    bool headersParsed_ = false;                     ///< 头部是否已被 HttpParser 解析完毕
    bool keepAlive_ = false;                         ///< 后端是否支持 keep-alive（从响应头解析）
    static constexpr int kMaxIdleSeconds = 50;       ///< 空闲超时阈值（秒），超过则认为连接不可复用
    std::chrono::steady_clock::time_point lastActiveTime_;  ///< 最近一次活跃时间（连接建立/数据收发/归还）
    std::string pendingRequestBody_;                 ///< 暂存待发送的 JSON 请求体（connect 前存入，握手后自动发送）
    std::string targethost_;                         ///< 目标域名，如"api.edgefn.net"
    int targetport_;                                 ///< 目标端口，如443

    ThreadPool* threadPool_;                         ///< 在线程池异步执行阻塞方法
};
} // namespace MyServer