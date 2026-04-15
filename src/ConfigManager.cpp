/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-14 11:11:07
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-14 11:31:30
 * @FilePath: /OmniGateway/src/ConfigManager.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "ConfigManager.hpp"
#include "json.hpp"
#include "Logger.hpp"
#include <fstream>

using json = nlohmann::json;

namespace MyServer {

bool ConfigManager::loadConfig(const std::string &configFilePath) {
    try {
        std::ifstream configFile(configFilePath);
        if (!configFile.is_open()) {
            LOG_ERROR << "无法打开配置文件: " << configFilePath;
            return false;
        }

        json configJson;
        configFile >> configJson;

        // 1. 加载 server 配置
        if (configJson.contains("server")) {
            const auto &serverJson = configJson["server"];
            serverConfig_.port = serverJson.value("port", 8080);
            serverConfig_.threadNum = serverJson.value("thread_num", 4);

            // 解析日志级别：INFO / WARNING / ERROR / FATAL
            std::string logLevelStr = serverJson.value("log_level", "INFO");
            if (logLevelStr == "WARNING" || logLevelStr == "WARN") {
                setLogLevel(LogLevel::WARNING);
            } else if (logLevelStr == "ERROR") {
                setLogLevel(LogLevel::ERROR);
            } else if (logLevelStr == "FATAL") {
                setLogLevel(LogLevel::FATAL);
            } else {
                setLogLevel(LogLevel::INFO);
            }
        } else {
            LOG_WARNING << "配置文件中未找到 server 配置，使用默认值";
        }

        // 2. 加载 api 配置
        if (configJson.contains("backend")) {
            const auto &apiJson = configJson["backend"];
            apiConfig_.host = apiJson.value("host", "");
            apiConfig_.path = apiJson.value("path", "");
            apiConfig_.apiKey = apiJson.value("api_key", "");
            apiConfig_.targetModel = apiJson.value("target_model", "");
        } else {
            LOG_ERROR << "配置文件中未找到 backend 配置，无法启动后端代理！";
            return false;
        }

        // 对 API Key 进行脱敏，只显示前 4 位和后 4 位，中间用 * 代替
        std::string maskedKey = apiConfig_.apiKey;
        if (maskedKey.length() > 8) {
            maskedKey = maskedKey.substr(0, 4) + "******" + maskedKey.substr(maskedKey.length() - 4);
        } else {
            maskedKey = "******"; // 如果长度太短，全遮挡
        }

        LOG_INFO << "配置文件加载成功！";
        LOG_INFO << "Server Port: " << serverConfig_.port;
        LOG_INFO << "Thread Num: " << serverConfig_.threadNum;
        LOG_INFO << "API Host: " << apiConfig_.host;
        LOG_INFO << "API Path: " << apiConfig_.path;
        LOG_INFO << "API Key: " << maskedKey;
        LOG_INFO << "Target Model: " << apiConfig_.targetModel;

        return true;
    } catch (const json::exception &e) {
        LOG_ERROR << "JSON 解析错误: " << e.what();
        return false;
    } catch (const std::exception &e) {
        LOG_ERROR << "配置文件加载失败: " << e.what();
        return false;
    }
}

} // namespace MyServer