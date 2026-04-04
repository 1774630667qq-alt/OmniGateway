/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-26 17:08:52
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-26 19:43:01
 * @FilePath: /ServerPractice/src/HttpParser.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "HttpParser.hpp"
#include "Logger.hpp"

namespace MyServer {
    bool HttpParser::parseHeaders(const std::string& headers_str, HttpRequest* request) {
        size_t index = 0;
        while (index + 1 < headers_str.size() && headers_str.substr(index, 2) != "\r\n") { // 遇到空行就结束
            // 找到下一行的结尾
            size_t next_line_pos = headers_str.find("\r\n", index);
            if (next_line_pos == std::string::npos) {
                LOG_WARNING << "解析请求头失败：没有找到行结束符";
                return false;
            }

            // 切出当前行
            std::string line = headers_str.substr(index, next_line_pos - index);
            index = next_line_pos + 2; // 跳过 "\r\n"

            // 找到冒号的位置
            size_t colon_pos = line.find(':');
            if (colon_pos == std::string::npos) {
                LOG_WARNING << "解析请求头失败：没有找到冒号分隔符";
                return false;
            }

            // 切出 Key 和 Value
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // 去掉 Value 前面的空格
            size_t first_non_space = value.find_first_not_of(' ');
            if (first_non_space != std::string::npos) {
                value = value.substr(first_non_space);
            } else {
                value = ""; // 如果全是空格，Value 就是空字符串
            }

            // 添加到请求对象中
            request->addHeader(key, value);
        }
        return true;
    }

    bool HttpParser::parseRequestLine(const std::string& line, HttpRequest* request) {
        // 找到第一个空格
        size_t first_space_pos = line.find(' ');
        if (first_space_pos == std::string::npos) {
            LOG_WARNING << "解析请求行失败：没有找到第一个空格";
            return false; // 没有第一个空格，格式错误
        }
        std::string method = line.substr(0, first_space_pos);

        // 找到第二个空格
        size_t second_space_pos = line.find(' ', first_space_pos + 1);
        if (second_space_pos == std::string::npos) {
            LOG_WARNING << "解析请求行失败：没有找到第二个空格";
            return false; // 没有第二个空格，格式错误
        }
        std::string path = line.substr(first_space_pos + 1, second_space_pos - first_space_pos - 1);
        std::string version = line.substr(second_space_pos + 1);

        // 判断版本号是否合法
        if (version.find("HTTP/") != 0) {
            return false;
        }

        request->setMethod(method);
        request->setPath(path);
        request->setVersion(version);
        return true;
    }

    bool HttpParser::parse(const std::string& raw_msg, HttpRequest* request) {
        // 找到请求行的结尾
        size_t request_line_end_pos = raw_msg.find("\r\n");
        if (request_line_end_pos == std::string::npos) {
            return false; // 没有找到请求行的结尾，格式错误
        }

        // 切出请求行
        std::string request_line = raw_msg.substr(0, request_line_end_pos);
        if (!parseRequestLine(request_line, request)) {
            return false; // 解析请求行失败
        }

        // 切出请求头部分
        std::string headers_str = raw_msg.substr(request_line_end_pos + 2);
        return parseHeaders(headers_str, request);
    }
} // namespace MyServer