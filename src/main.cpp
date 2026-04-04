/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-04 16:15:45
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-04 17:04:34
 * @FilePath: /OmniGateway/src/main.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "EventLoop.hpp"
#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include "HttpServer.hpp"
#include "Logger.hpp"
#include "ThreadPool.hpp"
#include "json.hpp"

using namespace MyServer;
using json = nlohmann::json;

/**
 * @brief 核心翻译函数：将 Anthropic 格式的请求体，转换为 OpenAI 格式
 * @param anthropic_str 原始的 Claude 请求 JSON 字符串
 * @param target_model  我们要转发给的真实大模型 ID (比如你的 GLM-5)
 * @return 转换后的 OpenAI 格式 JSON 字符串
 */
std::string translateAnthropicToOpenAI(const std::string &anthropic_str,
                                       const std::string &target_model) {
    // 1. 将普通字符串解析为极其强大的 C++ JSON 对象
    json anthropic_json = json::parse(anthropic_str);

    // 2. 创建一个空的 OpenAI JSON 对象，准备组装
    json openai_json;

    // 3. 强行替换 Model 为你白山智算的 GLM-5
    openai_json["model"] = target_model;

    // 4. 准备一个新的 messages 数组
    openai_json["messages"] = json::array();

    // 5. 【核心翻译逻辑】：处理 System Prompt 的差异
    // 如果 Anthropic 请求里带有 system 字段
    if (anthropic_json.contains("system")) {
        // 在 OpenAI 的 messages 最前面，塞入一条 role 为 system 的消息
        openai_json["messages"].push_back(
            {{"role", "system"}, {"content", anthropic_json["system"]}});
    }

    // 6. 把原本的 messages 原封不动地拷贝过来
    if (anthropic_json.contains("messages")) {
        for (const auto &msg : anthropic_json["messages"]) {
            openai_json["messages"].push_back(msg);
        }
    }

    // 7. 还可以把其他参数（比如 max_tokens, temperature）也拷贝过去
    // (这里做个简单的演示)
    if (anthropic_json.contains("temperature")) {
        openai_json["temperature"] = anthropic_json["temperature"];
    }

    // 8. 序列化：将 C++ JSON 对象重新变回人类可读的字符串，缩进为 4 个空格
    return openai_json.dump(4);
}

int main() {
    // 1. 初始化全局异步日志
    initGlobalLogger("OmniGateway");

    LOG_INFO << "Starting OmniGateway...";

    // 2. 创建主 Reactor
    EventLoop loop;

    // 3. 创建业务线程池，处理具体请求
    ThreadPool pool(8);

    // 4. 创建 HttpServer
    HttpServer server(&loop, 8080, &pool);

    // 设置网络 IO 线程数 (Sub Reactors)
    server.setThreadNum(4);

    // 5. 注册路由回调
    server.setHttpCallback([](const HttpRequest &req, HttpResponse &resp) {
        LOG_INFO << "Received HTTP request";

        resp.setStatusCode(200, "OK");
        resp.addHeader("Content-Type", "text/plain");
        resp.setBody("Welcome to OmniGateway!\n");
    });

    // 6. 启动服务器与事件循环
    server.start();
    LOG_INFO << "OmniGateway is listening on port 8080...";

    loop.loop();

    return 0;
}
