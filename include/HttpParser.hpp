/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-26 17:07:45
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-12 14:52:00
 * @FilePath: /OmniGateway/include/HttpParser.hpp
 * @Description: 通用 HTTP 协议解析器，支持请求模式和响应模式
 */
#pragma once
#include "HttpRequest.hpp"
#include "Buffer.hpp"
#include <string>
#include <unordered_map>

namespace MyServer {

/**
 * @class HttpParser
 * @brief 通用 HTTP 协议解析器：同时支持解析客户端请求 (Request) 和服务端响应 (Response)
 * @details
 *   [架构角色]
 *   在 OmniGateway 的双向代理架构中，HttpParser 承担两种解析职责：
 *   - 请求模式 (kRequestMode)：由 HttpServer 使用，解析前端用户发来的 HTTP 请求
 *   - 响应模式 (kResponseMode)：由 HttpClient 使用，解析后端大模型 API 返回的 HTTP 响应
 *
 *   [设计]
 *   解析器内部维护一个有限状态机 (FSM)，根据工作模式从不同的初始状态出发：
 *   - 请求模式：kExpectRequestLine → kExpectHeaders → kGotAll
 *   - 响应模式：kExpectResponseLine → kExpectHeaders → kGotAll
 *   解析过程是增量式的（流式友好），每次调用 parse() 会尽可能多地消费 Buffer 中的数据，
 *   数据不够时返回 true 并保持当前状态，等待下次数据到达后继续解析。
 *
 *   [向后兼容]
 *   保留了原有的静态方法 parse(const std::string&, HttpRequest*)，
 *   HttpServer 中的调用代码无需做任何修改。
 */
class HttpParser {
public:
    /**
     * @brief 解析器工作模式
     */
    enum class ParserMode {
        kRequestMode,  ///< 解析客户端发来的 HTTP 请求（默认）
        kResponseMode  ///< 解析服务端返回的 HTTP 响应
    };

    /**
     * @brief 构造函数：创建指定工作模式的解析器实例
     * @signature explicit HttpParser(ParserMode mode = ParserMode::kRequestMode);
     * @param mode 解析器工作模式，默认为请求模式（兼容 HttpServer 中的无参构造）
     * @details
     *   根据传入的模式设置状态机初始状态：
     *   - kRequestMode  → 状态机从 kExpectRequestLine 开始
     *   - kResponseMode → 状态机从 kExpectResponseLine 开始
     */
    explicit HttpParser(ParserMode mode = ParserMode::kRequestMode)
        : mode_(mode),
          state_(mode == ParserMode::kRequestMode ? kExpectRequestLine : kExpectResponseLine) {}

    // ====================== 增量式解析接口（流式友好）======================

    /**
     * @brief 增量式解析入口：从 Buffer 中逐步消费并解析 HTTP 报文
     * @signature bool parse(Buffer* buf);
     * @param buf 应用层读缓冲区指针，包含从网络收到的原始数据
     * @details
     *   [工作流程]
     *   在 while 循环中驱动状态机前进，每次迭代尝试解析当前状态期望的内容：
     *   1. kExpectRequestLine / kExpectResponseLine：查找 \r\n，解析首行
     *   2. kExpectHeaders：逐行解析 "Key: Value\r\n"，遇到空行 \r\n 表示头部结束
     *   3. kGotAll：解析完成
     * 
     *   [流式友好]
     *   如果数据不足（如只收到半行），函数返回 true 并保持当前状态，
     *   下次数据到达后可以再次调用继续解析。只有遇到格式错误才返回 false。
     * 
     * @return true 表示解析正常（可能尚未完成，需检查 gotAll()）；false 表示报文格式错误
     */
    bool parse(Buffer* buf);

    // ====================== 静态解析接口（向后兼容）======================

    /**
     * @brief 静态解析入口：一次性解析完整的 HTTP 请求头部字符串（向后兼容 HttpServer）
     * @signature static bool parse(const std::string& raw_msg, HttpRequest* request);
     * @param raw_msg 底层传来的包含 \r\n\r\n 的完整头部字符串
     * @param request 要被填充的请求对象指针
     * @return 解析成功返回 true，格式错误返回 false
     */
    static bool parse(const std::string& raw_msg, HttpRequest* request);

    // ====================== 状态查询接口 ======================

    /**
     * @brief 判断头部解析是否全部完成
     * @return true 表示已解析完所有头部（状态机处于 kGotAll）
     */
    bool gotAll() const { return state_ == kGotAll; }

    /**
     * @brief 获取解析到的 HTTP 响应状态码（仅在 kResponseMode 下有效）
     * @return HTTP 状态码（如 200, 401, 500），未解析到则为 0
     */
    int statusCode() const { return statusCode_; }

    /**
     * @brief 获取解析到的指定 HTTP 头部字段值
     * @param key 头部字段名（如 "Content-Length", "Transfer-Encoding"）
     * @return 对应的值字符串；若不存在则返回空字符串
     */
    std::string getHeader(const std::string& key) const {
        auto it = headers_.find(key);
        return it != headers_.end() ? it->second : "";
    }

    /**
     * @brief 重置解析器状态，可复用于解析下一个报文
     */
    void reset() {
        state_ = (mode_ == ParserMode::kRequestMode) ? kExpectRequestLine : kExpectResponseLine;
        statusCode_ = 0;
        headers_.clear();
    }

private:
    /**
     * @brief 解析器内部状态机枚举
     * @details
     *   状态迁移路径：
     *   - 请求模式：kExpectRequestLine → kExpectHeaders → kGotAll
     *   - 响应模式：kExpectResponseLine → kExpectHeaders → kGotAll
     */
    enum ParseState {
        kExpectRequestLine,  ///< 期待 HTTP 请求行（如 "POST /v1/chat HTTP/1.1"）
        kExpectResponseLine, ///< 期待 HTTP 响应行（如 "HTTP/1.1 200 OK"）
        kExpectHeaders,      ///< 期待 HTTP 头部键值对
        kGotAll              ///< 解析完成（头部已全部提取）
    };

    // ====================== 内部解析辅助方法 ======================

    /**
     * @brief 解析请求行
     * @param line 形如 "GET /index.html HTTP/1.1" 的单行字符串（不含 \r\n）
     * @param request 要被填充的请求对象指针
     * @return 成功 true，失败 false
     */
    static bool parseRequestLine(const std::string& line, HttpRequest* request);

    /**
     * @brief 解析所有的请求头（静态接口专用）
     * @param headers_str 剥离了请求行之后的、包含多个 "Key: Value\r\n" 的长字符串
     * @param request 要被填充的请求对象指针
     * @return 成功 true，失败 false
     */
    static bool parseHeaders(const std::string& headers_str, HttpRequest* request);

    /**
     * @brief 解析响应行，提取状态码
     * @signature bool parseResponseLine(const std::string& line);
     * @param line 形如 "HTTP/1.1 200 OK" 的单行字符串（不含 \r\n）
     * @return 成功 true，失败 false
     * @details
     *   [解析规则]
     *   响应行格式为 "版本 状态码 原因短语"，以空格分隔。
     *   本方法提取第一个和第二个空格之间的数字字符串，转换为整型状态码存入 statusCode_。
     */
    bool parseResponseLine(const std::string& line);

    /**
     * @brief 解析单行头部键值对并存入 headers_ 映射表
     * @param line 形如 "Content-Type: application/json" 的单行字符串（不含 \r\n）
     * @return 成功 true，失败 false
     */
    bool parseHeaderLine(const std::string& line);

    // ====================== 成员变量 ======================

    ParserMode mode_;                                          ///< 工作模式（请求 / 响应）
    ParseState state_;                                         ///< 当前状态机状态
    int statusCode_ = 0;                                       ///< HTTP 响应状态码（响应模式下有效）
    std::unordered_map<std::string, std::string> headers_;     ///< 解析到的头部键值对映射表
};

} // namespace MyServer