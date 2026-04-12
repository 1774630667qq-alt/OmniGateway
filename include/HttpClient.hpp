/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-08 15:56:09
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-11 22:22:08
 * @FilePath: /OmniGateway/include/HttpClient.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "EventLoop.hpp"
#include "TcpConnection.hpp"
#include "Connector.hpp"
#include "HttpParser.hpp"

namespace MyServer {

/**
 * @brief 后端 API 连接配置结构体
 * @details 封装了与一个大模型 API 后端节点通信所需的全部参数。
 *   在反向代理场景中，OmniGateway 收到前端用户请求后，会根据此配置
 *   构建 HTTP 请求报文并发往对应的后端大模型服务。
 * 
 *   各字段说明：
 *   - host:        后端 API 的域名或 IP（用于 HTTP Host 头和 DNS 解析），例如 "api.edgefn.net"
 *   - path:        请求的 URI 路径（如 "/v1/chat/completions"）
 *   - apiKey:      后端 API 的鉴权密钥（如 "sk-..."），将被填入 Authorization 头
 *   - targetModel: 要调用的目标模型标识（如 "GLM-5"），将被填入请求体的 model 字段
 */
struct ApiConfig {
    std::string host;       ///< 后端 API 域名，例如 "api.edgefn.net"
    std::string path;       ///< 请求路径，例如 "/v1/chat/completions"
    std::string apiKey;     ///< 鉴权密钥，例如 "sk-..."
    std::string targetModel;///< 目标模型标识，例如 "GLM-5"
};

/**
 * @class HttpClient
 * @brief 反向代理的客户端枢纽：负责与后端大模型 API 建立连接、发送请求、并解析流式响应
 * @details
 *   [架构角色]
 *   在 OmniGateway 的反向代理架构中，HttpClient 处于"下游客户端"的角色。
 *   当前端用户通过 HttpServer 发来请求后，业务层会创建一个 HttpClient 实例，
 *   由它主动连接后端大模型 API 节点（如智谱、OpenAI），转发请求并将响应回传。
 * 
 *   [生命周期]
 *   1. 构造阶段：接收 EventLoop、目标地址和 API 配置，内部创建 Connector 但不立即发起连接。
 *   2. 连接阶段：调用 connect() 后，Connector 在 IO 线程中以非阻塞方式发起 TCP 三次握手。
 *   3. TLS 握手：TCP 连通后，由 TcpConnection::doTlsHandshake() 完成 SSL 加密通道建立。
 *   4. 数据交互：握手完成后，通过 sendRequest() 发送 HTTP 请求，onMessage() 接收后端响应。
 *   5. 销毁阶段：连接断开或出错时，onClose() 被触发，清理 TcpConnection 资源。
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

    /**
     * @brief 构造函数：初始化客户端，绑定事件循环与连接器
     * @signature HttpClient(EventLoop* loop, const InetAddress& serverAddr, const ApiConfig& apiConfig);
     * @param loop 当前 HttpClient 将要运行在的 EventLoop（IO 线程）
     * @param serverAddr 后端大模型 API 服务的网络地址（IP + 端口）
     * @param apiConfig 后端 API 的配置信息（域名、路径、密钥、模型名）
     * @details
     *   构造阶段仅创建 Connector 实例并保存配置，不会立即发起网络连接。
     *   必须显式调用 connect() 才会启动连接状态机。
     */
    HttpClient(EventLoop* loop, const InetAddress& serverAddr, const ApiConfig& apiConfig);

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
     * @brief 组装并发送完整的 HTTP POST 请求
     * @signature void sendRequest(const std::string& requestBody);
     * @param requestBody 已经转换好的 OpenAI 格式的 JSON 字符串
     * @details
     * [职责]
     * 纯粹的 JSON 字符串无法直接发给大模型 API，必须被包装成合法的 HTTP 报文。
     * 该方法负责手工拼接 HTTP 请求行（POST /v1/...）、请求头（Host, Content-Type, Authorization, Content-Length）
     * 以及空行（\r\n），最后附上 requestBody，统一发往底层的 backendConn_。
     * * [前置条件]
     * 调用此方法前，必须确保 connected_ 为 true 且 TLS 握手已成功完成。
     */
    void sendRequest(const std::string& requestBody);

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

private:
    /**
     * @brief Connector 连接成功后的交接回调（待实现）
     * @signature void onConnection(int sockfd);
     * @param sockfd Connector 成功建立 TCP 连接后移交的已连通套接字文件描述符
     * @details
     *   [设计契约]
     *   实现时应当完成以下工作：
     *   1. 基于 sockfd 创建 TcpConnection 对象（存入 backendConn_）。
     *   2. 为 backendConn_ 注册 onMessage 和 onClose 回调。
     *   3. 调用 backendConn_->connectEstablished() 将 fd 注册进 epoll 监听。
     *   4. 更新 connected_ 状态为 true。
     *   此回调标志着从"修路阶段"进入"通车阶段"。
     */
    void onConnection(int sockfd);

    void onMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);

    void onClose(const std::shared_ptr<TcpConnection>& conn);

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
    
    bool connected_ = false;                         ///< 当前是否与后端 API 处于已连接状态
    bool headersParsed_ = false;                     ///< 头部是否已被 HttpParser 解析完毕
};
} // namespace MyServer