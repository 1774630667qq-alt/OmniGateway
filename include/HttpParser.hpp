/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-26 17:07:45
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-26 17:37:24
 * @FilePath: /ServerPractice/include/HttpParser.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include "HttpRequest.hpp"
#include <string>

namespace MyServer {

/**
 * @brief HTTP 报文解析器 (无情的数据切割机)
 * @details 它的任务只有一个：把纯字符串转换成 HttpRequest 对象。
 * 因为它不需要保存状态，我们把它设计成包含全静态方法的工具类。
 */
class HttpParser {
public:
    /**
     * @brief 核心入口：解析整个 HTTP 头部字符串
     * @param raw_msg 底层传来的包含 \r\n\r\n 的完整头部字符串
     * @param request 要被填充的请求对象指针
     * @return 解析成功返回 true，如果有语法错误返回 false
     */
    static bool parse(const std::string& raw_msg, HttpRequest* request);

private:
    /**
     * @brief 解析请求行
     * @param line 形如 "GET /index.html HTTP/1.1" 的单行字符串（不含 \r\n）
     * @param request 
     * @return 成功 true，失败 false
     */
    static bool parseRequestLine(const std::string& line, HttpRequest* request);

    /**
     * @brief 解析所有的请求头
     * @param headers_str 剥离了请求行之后的、包含多个 "Key: Value\r\n" 的长字符串
     * @param request 
     * @return 成功 true，失败 false
     */
    static bool parseHeaders(const std::string& headers_str, HttpRequest* request);
};

} // namespace MyServer