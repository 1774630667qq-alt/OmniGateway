/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-04 16:15:45
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-12 21:58:14
 * @FilePath: /OmniGateway/src/main.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "EventLoop.hpp"
#include "HttpClient.hpp"
#include "SSLManager.hpp"
#include "Logger.hpp"
#include <iostream>
#include <signal.h>

using namespace MyServer;

int main() {
    // 1. 忽略 SIGPIPE 信号，防止连接异常断开导致进程直接崩溃退出
    signal(SIGPIPE, SIG_IGN);

    // 2. 正确调用全局日志初始化函数
    MyServer::initGlobalLogger("SmokeTestLog");

    // 3. 正确初始化 OpenSSL 环境
    SSLManager::init();

    EventLoop loop;

    // 4. 配置大模型 API 参数
    ApiConfig config;
    config.host = "api.edgefn.net"; 
    config.path = "/v1/chat/completions";
    config.apiKey = "sk-xxx"; // 【务必修改】填入你的真实鉴权密钥
    config.targetModel = "GLM-5"; 

    // 5. 目标地址
    // 注意：如果你的 InetAddress 还没实现 DNS 解析，请先 ping api.edgefn.net 
    // 获取真实 IP（例如 "114.114.x.x"），然后填在这里。
    InetAddress serverAddr(443, "198.18.0.87"); 

    // 6. 实例化 HttpClient
    std::shared_ptr<HttpClient> client = std::make_shared<HttpClient>(&loop, serverAddr, config);

    // 7. 挂载响应回调：直接在控制台打印状态机切割出的纯净 SSE 碎片
    client->setResponseCallback([](const std::string& data) {
        std::cout << "\n\033[32m[完美切割的 SSE 事件]\033[0m\n";
        std::cout << data << std::endl;
    });

    // 8. 发起异步连接 (非阻塞)
    client->connect();

    // 9. 延迟发送请求 (模拟 ConnectionCallback)
    // 这里的延时用于等待底层 TCP 和 TLS 握手彻底完成。
    // 【注意】：如果你的 runAfter 接收的是秒（double），请写 2.0；如果是毫秒（int），请写 2000。
    loop.runAfter(2.0, [client, config]() {
        LOG_INFO << "========= 延时结束，开始发射 HTTP 报文 =========";
        
        // 构造一个标准的 OpenAI 格式的测试 JSON
        std::string testJson = 
            "{\n"
            "  \"model\": \"" + config.targetModel + "\",\n"
            "  \"messages\": [{\"role\": \"user\", \"content\": \"你好，请用10个字以内介绍你自己。\"}],\n"
            "  \"stream\": true\n"
            "}";
        client->setRequestBody(testJson);
    });

    LOG_INFO << "冒烟测试程序启动，正在连接大模型 API...";
    
    // 10. 开启事件循环，驱动底层网络状态机
    loop.loop();

    // 11. 清理 SSL 资源
    SSLManager::destroy();

    return 0;
}