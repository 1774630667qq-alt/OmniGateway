/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-26 17:41:20
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-31 22:47:51
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
            if (buffer->findCRLF() == std::string::npos) { // 半包，继续等待
                return;
            }

            LOG_INFO << "开始解释HTTP请求...";

            // 解析文件头
            HttpParser parser;
            HttpRequest request;

            if (!parser.parse(buffer->extractHttpHeaders(), &request)) {
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
                if (buffer->size() < content_length + buffer->findCRLF() + 4) {
                    // 还没收全，继续等待
                    return;
                }
                request.setBody(std::string(buffer->data() + buffer->findCRLF() + 4, content_length));
            } 

            // 已经完成首个 HTTP 请求的解析，接下来要把该请求从 Buffer 中删掉，准备解析下一个请求
            size_t total_request_length = buffer->findCRLF() + 4 + request.getBody().size();
            buffer->retrieve(total_request_length); // 从 Buffer 中删掉已经处理完的请求

            HttpResponse response;
            pool_->enqueue([cb, conn, request, response]() mutable {
                if (cb) {
                    cb(request, response); // 业务层处理请求，填充响应
                }

                bool isFile = response.isFile();
                std::string response_str = isFile ? response.assembleHeaders() : response.assemble(); // 如果是文件，只拼装头部，文件内容留给 TcpConnection 的 sendfile 去发
                std::string filePath = response.getFilePath();

                EventLoop* loop = conn->getLoop();
                loop->queueInLoop([conn, response_str, isFile, filePath]() {
                    conn->send(response_str); // 先发响应行和响应头
                    if (isFile) {
                        conn->sendFile(filePath); // 再发文件内容
                    }
                });
                
            });
        }
    }
} // namespace MyServer