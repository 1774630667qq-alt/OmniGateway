/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-04 16:15:45
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-04 18:08:09
 * @FilePath: /OmniGateway/src/main.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "EventLoop.hpp"
#include "HttpServer.hpp"
#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include "ThreadPool.hpp"
#include "Logger.hpp"
#include "json.hpp"
#include <string>
#include <signal.h>

using namespace MyServer;
using json = nlohmann::json;

// 你亲手写的核心翻译官
std::string translateAnthropicToOpenAI(const std::string& anthropic_str, 
                                    const std::string& target_model) {
    try {
        json anthropic_json = json::parse(anthropic_str);
        json openai_json;
        openai_json["model"] = target_model;
        openai_json["messages"] = json::array();
        
        if (anthropic_json.contains("system")) {
            openai_json["messages"].push_back({
                {"role", "system"},
                {"content", anthropic_json["system"]}
            });
        }
        
        if (anthropic_json.contains("messages")) {
            for (const auto& msg : anthropic_json["messages"]) {
                openai_json["messages"].push_back(msg);
            }
        }
        
        if (anthropic_json.contains("temperature")) {
            openai_json["temperature"] = anthropic_json["temperature"];
        }
        
        return openai_json.dump(); // 实际网络传输，不要空格，极致压缩！
    } catch (const std::exception& e) {
        LOG_ERROR << "JSON 解析/转换失败: " << e.what();
        return ""; // 如果传过来的不是合法 JSON，防崩溃
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN); // 忽略断开信号，防崩溃
    MyServer::initGlobalLogger("OmniGatewayLog");

    EventLoop loop;
    ThreadPool pool(4); // 网关主要做 IO 转发，工作线程不需要太多，4 个足矣

    // 在 8080 端口启动我们的代理网关
    HttpServer gateway_server(&loop, 8080, &pool);
    gateway_server.setThreadNum(2); 

    gateway_server.setHttpCallback([](const HttpRequest& req, HttpResponse& res) {
        LOG_INFO << "收到来自客户端的请求: " << req.getMethod() << " " << req.getPath();

        // 拦截 Claude Code 发往 Anthropic 官方的默认接口地址：/v1/messages
        if (req.getPath() == "/v1/messages" && req.getMethod() == "POST") {
            
            // 1. 从 HTTP 请求体中掏出 Claude 给我们的原始数据
            std::string claude_body = req.getBody();
            LOG_INFO << "接收到 Claude 的数据，长度: " << claude_body.size() << " bytes";

            // 2. 调用翻译引擎 (目标设为白山智算的 GLM-5)
            std::string openai_body = translateAnthropicToOpenAI(claude_body, "GLM-5");

            if (openai_body.empty()) {
                res.setStatusCode(400, "Bad Request");
                res.setBody("{\"error\": \"Invalid JSON Format\"}");
                res.addHeader("Content-Type", "application/json");
                return;
            }

            // 3. (暂不发往白山智算) 我们先把翻译好的数据直接作为 HTTP 响应返回，方便调试！
            res.setStatusCode(200, "OK");
            res.addHeader("Content-Type", "application/json");
            res.setBody(openai_body);
            
            LOG_INFO << "成功完成协议翻译并返回响应！";
        } 
        else {
            // 对付那些乱请求的路径
            res.setStatusCode(404, "Not Found");
            res.setBody("OmniGateway: Path not supported yet.");
        }
    });

    LOG_INFO << "OmniGateway 引擎启动成功！正在监听 8080 端口...";
    gateway_server.start();
    loop.loop();

    return 0;
}