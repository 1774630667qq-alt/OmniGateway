/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-26 17:08:52
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-12 14:52:00
 * @FilePath: /OmniGateway/src/HttpParser.cpp
 * @Description: 通用 HTTP 协议解析器实现
 */
#include "HttpParser.hpp"
#include "Logger.hpp"

namespace MyServer {

    // ====================== 增量式解析实现 ======================

    bool HttpParser::parse(Buffer* buf) {
        bool ok = true;
        bool hasMore = true;

        while (hasMore) {
            if (state_ == kExpectRequestLine) {
                // 查找行结束标志 "\r\n"
                size_t crlfPos = buf->findCRLF(std::string_view("\r\n", 2));
                if (crlfPos != std::string::npos) {
                    std::string requestLine(buf->peek(), crlfPos);
                    // 创建临时 HttpRequest 来复用原有的 parseRequestLine
                    // （请求模式下，调用方应该直接用静态接口，此处仅作兼容）
                    HttpRequest tmpReq;
                    ok = parseRequestLine(requestLine, &tmpReq);
                    if (!ok) {
                        LOG_WARNING << "HTTP 请求行解析失败: " << requestLine;
                        return false;
                    }
                    buf->retrieve(crlfPos + 2); // 消费请求行 + \r\n
                    state_ = kExpectHeaders;
                } else {
                    hasMore = false; // 数据不够一行，等待更多数据
                }
            }
            else if (state_ == kExpectResponseLine) {
                // 查找行结束标志 "\r\n"
                size_t crlfPos = buf->findCRLF(std::string_view("\r\n", 2));
                if (crlfPos != std::string::npos) {
                    std::string responseLine(buf->peek(), crlfPos);
                    ok = parseResponseLine(responseLine);
                    if (!ok) {
                        LOG_WARNING << "HTTP 响应行解析失败: " << responseLine;
                        return false;
                    }
                    buf->retrieve(crlfPos + 2); // 消费响应行 + \r\n
                    state_ = kExpectHeaders;
                } else {
                    hasMore = false;
                }
            }
            else if (state_ == kExpectHeaders) {
                size_t crlfPos = buf->findCRLF(std::string_view("\r\n", 2));
                if (crlfPos != std::string::npos) {
                    if (crlfPos == 0) {
                        // 空行 "\r\n"：头部解析结束
                        buf->retrieve(2); // 消费空行的 \r\n
                        state_ = kGotAll;
                        hasMore = false;
                    } else {
                        // 解析一行头部 "Key: Value"
                        std::string headerLine(buf->peek(), crlfPos);
                        ok = parseHeaderLine(headerLine);
                        if (!ok) {
                            LOG_WARNING << "HTTP 头部行解析失败: " << headerLine;
                            return false;
                        }
                        buf->retrieve(crlfPos + 2); // 消费该头部行 + \r\n
                    }
                } else {
                    hasMore = false;
                }
            }
            else if (state_ == kGotAll) {
                hasMore = false;
            }
        }
        return ok;
    }

    // ====================== 静态解析实现（向后兼容 HttpServer）======================

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

    // ====================== 增量式解析辅助方法 ======================

    bool HttpParser::parseResponseLine(const std::string& line) {
        // 响应行格式: "HTTP/1.1 200 OK"
        // 找到第一个空格（版本号结束）
        size_t space1 = line.find(' ');
        if (space1 == std::string::npos) {
            LOG_WARNING << "解析响应行失败：没有找到第一个空格";
            return false;
        }

        // 验证版本号
        std::string version = line.substr(0, space1);
        if (version.find("HTTP/") != 0) {
            LOG_WARNING << "解析响应行失败：版本号格式不合法: " << version;
            return false;
        }

        // 找到第二个空格（状态码结束）
        size_t space2 = line.find(' ', space1 + 1);
        if (space2 == std::string::npos) {
            // 有些响应可能没有 Reason Phrase（如 "HTTP/1.1 200"），兼容处理
            std::string codeStr = line.substr(space1 + 1);
            try {
                statusCode_ = std::stoi(codeStr);
            } catch (...) {
                LOG_WARNING << "解析响应行失败：状态码转换失败: " << codeStr;
                return false;
            }
            return true;
        }

        // 提取状态码
        std::string codeStr = line.substr(space1 + 1, space2 - space1 - 1);
        try {
            statusCode_ = std::stoi(codeStr);
        } catch (...) {
            LOG_WARNING << "解析响应行失败：状态码转换失败: " << codeStr;
            return false;
        }

        return true;
    }

    bool HttpParser::parseHeaderLine(const std::string& line) {
        // 头部行格式: "Key: Value"
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            LOG_WARNING << "解析头部行失败：没有找到冒号分隔符";
            return false;
        }

        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);

        // 去掉 Value 前面的空格
        size_t first_non_space = value.find_first_not_of(' ');
        if (first_non_space != std::string::npos) {
            value = value.substr(first_non_space);
        } else {
            value = "";
        }

        headers_[key] = value;
        return true;
    }

} // namespace MyServer