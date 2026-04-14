/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-14 11:11:07
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-14 11:25:01
 * @FilePath: /OmniGateway/include/ConfigManager.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <string>

namespace MyServer {
// 1. 定义前端网关的配置结构体
struct ServerConfig {
    int port;
    int threadNum;
};

/**
 * @brief 后端 API 连接配置结构体
 * @details 封装了与一个大模型 API 后端节点通信所需的全部参数。
 *   在反向代理场景中，OmniGateway 收到前端用户请求后，会根据此配置
 *   构建 HTTP 请求报文并发往对应的后端大模型服务。
 *
 *   各字段说明：
 *   - host:        后端 API 的域名或 IP（用于 HTTP Host 头和 DNS 解析），例如
 * "api.edgefn.net"
 *   - path:        请求的 URI 路径（如 "/v1/chat/completions"）
 *   - apiKey:      后端 API 的鉴权密钥（如 "sk-..."），将被填入 Authorization
 * 头
 *   - targetModel: 要调用的目标模型标识（如 "GLM-5"），将被填入请求体的 model
 * 字段
 */
struct ApiConfig {
    std::string host;        ///< 后端 API 域名，例如 "api.edgefn.net"
    std::string path;        ///< 请求路径，例如 "/v1/chat/completions"
    std::string apiKey;      ///< 鉴权密钥，例如 "sk-..."
    std::string targetModel; ///< 目标模型标识，例如 "GLM-5"
};

class ConfigManager {
public:
    // 【方法 1】：获取全局唯一的单例实例
    static ConfigManager &getInstance() {
        static ConfigManager instance; // C++11 保证这里的初始化是线程安全的
        return instance;
    }

    // 删除拷贝构造和赋值操作，确保单例的绝对纯洁性
    ConfigManager(const ConfigManager &) = delete;
    ConfigManager &operator=(const ConfigManager &) = delete;

    // 【方法 2】：核心加载方法，在 main 函数最开头调用
    // 作用：打开 JSON 文件，读取内容，并使用 nlohmann::json 解析到结构体中
    bool loadConfig(const std::string &configFilePath);

    // 【方法 3 & 4】：业务层获取配置的只读接口
    const ServerConfig &getServerConfig() const { return serverConfig_; }
    const ApiConfig &getApiConfig() const { return apiConfig_; }

private:
    // 私有化构造函数 (单例模式的硬性要求)
    ConfigManager() = default;
    ~ConfigManager() = default;

    ServerConfig serverConfig_;
    ApiConfig apiConfig_;
};

} // namespace MyServer