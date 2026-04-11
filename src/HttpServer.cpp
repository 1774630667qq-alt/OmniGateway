/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-26 17:41:20
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-11 17:05:40
 * @FilePath: /ServerPractice/src/HttpServer.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "HttpServer.hpp"
#include "HttpParser.hpp"
#include "TcpConnection.hpp"
#include "EventLoop.hpp"
#include "Logger.hpp"
#include <string>

namespace MyServer {
    HttpServer::HttpServer(EventLoop* loop, int port, ThreadPool* pool)
        : server_(loop, port), pool_(pool) {
        // 将 onMessage 绑定为 httpCallback_，让 TcpServer 在收到消息时调用 HttpServer::onMessage
        server_.setOnMessageCallback([this](std::shared_ptr<TcpConnection> conn, Buffer* buffer) {
            this->onMessage(conn, buffer);
        });
    }

    void HttpServer::onMessage(std::shared_ptr<TcpConnection> conn, Buffer* buffer) {
        auto cb = httpCallback_; // 先把回调函数复制一份到栈上，防止在后续的异步操作中被修改或销毁
        while (true) {
            // 在可读数据中查找 HTTP 头部结束标志 "\r\n\r\n"
            size_t crlfPos = buffer->findCRLF();
            if (crlfPos == std::string::npos) { // 半包，继续等待
                return;
            }

            LOG_INFO << "开始解析HTTP请求...";

            // 头部长度 = crlfPos（"\r\n\r\n" 之前的字节数） + 4（"\r\n\r\n" 本身）
            size_t headerLen = crlfPos + 4;

            // 从 peek() 提取完整的 HTTP 头部字符串（不移动读游标）
            std::string headerStr(buffer->peek(), headerLen);

            // 解析文件头
            HttpParser parser;
            HttpRequest request;

            if (!parser.parse(headerStr, &request)) {
                HttpResponse response;
                response.setStatusCode(400, "Bad Request");
                response.setBody("400 Bad Request");
                conn->send(response.assemble());
                LOG_ERROR << "Failed to parse HTTP request. Sent 400 Bad Request.";
                // 错误请求，直接关闭连接
                conn->forceClose();
                return;
            }

            // 验证是否完整收到了请求体（如果有 Content-Length）
            if (request.findHeader("Content-Length")) {
                size_t content_length = 0;
                try {
                    content_length = std::stoul(request.getHeader("Content-Length"));
                } catch (const std::exception& e) {
                    LOG_ERROR << "Failed to parse Content-Length: " << e.what();
                    HttpResponse response;
                    response.setStatusCode(400, "Bad Request");
                    response.setBody("400 Bad Request");
                    conn->send(response.assemble());
                    conn->forceClose();
                    return;
                }
                if (buffer->readableBytes() < headerLen + content_length) {
                    // 头部 + body 还没收全，继续等待
                    return;
                }
                // 从 peek() + headerLen 的位置提取请求体
                request.setBody(std::string(buffer->peek() + headerLen, content_length));
            } 

            // 已经完成首个 HTTP 请求的解析，接下来要把该请求从 Buffer 中删掉，准备解析下一个请求
            size_t total_request_length = headerLen + request.getBody().size();
            buffer->retrieve(total_request_length); // 从 Buffer 中删掉已经处理完的请求

            HttpResponse response;
            pool_->enqueue([cb, conn, request, response]() mutable {
                if (cb) {
                    cb(request, response); // 业务层处理请求，填充响应
                }
            });
        }
    }
} // namespace MyServer