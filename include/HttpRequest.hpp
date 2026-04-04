/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-26 16:50:05
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-03 18:12:18
 * @FilePath: /ServerPractice/include/HttpRequest.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <string>
#include <unordered_map>

namespace MyServer {

/**
 * @brief HTTP 请求实体类：将浏览器发来的纯文本字符串，拆解为结构化的 C++ 对象
 */
class HttpRequest {
private:
    std::string method_;    ///< 请求方法，如 "GET", "POST"
    std::string path_;      ///< 请求路径，如 "/index.html"
    std::string version_;   ///< 协议版本，如 "HTTP/1.1"
    
    ///< 请求头字典，如 {"Host": "127.0.0.1", "Connection": "keep-alive"}
    std::unordered_map<std::string, std::string> headers_; 
    
    std::string body_;      ///< 请求体（对于 GET 通常为空，POST 存放提交的数据）

public:
    HttpRequest(): method_(""), path_(""), version_("") {};
    ~HttpRequest() = default;

    // --- Setter ---
    void setMethod(const std::string& method) { method_ = method; }
    void setPath(const std::string& path) { path_ = path; }
    void setVersion(const std::string& version) { version_ = version; }
    void addHeader(const std::string& key, const std::string& value) { headers_[key] = value; }
    void setBody(const std::string& body) { body_ = body; }
    bool findHeader(const std::string& key) const {
        return headers_.find(key) != headers_.end();
    }


    // --- Getter ---
    std::string getMethod() const { return method_; }

    std::string getPath() const { 
        return path_;
    }

    std::string getVersion() const { return version_; }

    std::string getHeader(const std::string& key) const {
        auto it = headers_.find(key);
        if (it != headers_.end()) {
            return it->second;
        }
        return ""; // 如果没找到这个请求头，返回空字符串
    }

    std::string getBody() const { 
        return body_; 
    }

    std::string getCookie(const std::string& key) const {
        auto it = headers_.find("Cookie");
        if (it != headers_.end()) {
            const std::string& cookie = it->second;
            size_t start = cookie.find(key + "=");
            if (start != std::string::npos) {
                start += key.size() + 1;
                size_t end = cookie.find(";", start);
                if (end != std::string::npos) {
                    return cookie.substr(start, end - start);
                }
                return cookie.substr(start);
            }
        }
        return "";
    }

    std::string getuserbybody() const {
        if (body_.find("user=") != std::string::npos) {
            size_t start = body_.find("user=") + 5;
            size_t end = body_.find("&", start);
            if (end != std::string::npos) {
                return body_.substr(start, end - start);
            }
            return body_.substr(start);
        }
        return "";
    }

    std::string getpwdbybody() const {
        if (body_.find("pwd=") != std::string::npos) {
            size_t start = body_.find("pwd=") + 4;
            size_t end = body_.find("&", start);
            if (end != std::string::npos) {
                return body_.substr(start, end - start);
            }
            return body_.substr(start);
        }
        return "";
    }
};

} // namespace MyServer