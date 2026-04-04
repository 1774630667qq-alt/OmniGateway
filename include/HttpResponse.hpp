/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-26 16:50:09
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-04 13:11:25
 * @FilePath: /ServerPractice/include/HttpResponse.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <string>
#include <unordered_map>
#include <sys/stat.h> // 用于获取文件信息
#include "Logger.hpp"

namespace MyServer {

/**
 * @brief HTTP 响应实体类：业务层只需要设置状态码和内容，它会自动组装成标准的 HTTP 报文
 */
class HttpResponse {
private:
    std::string version_;           ///< 协议版本，默认 "HTTP/1.1"
    int statusCode_;                ///< 状态码，如 200, 404
    std::string statusMessage_;     ///< 状态信息，如 "OK", "Not Found"
    
    std::unordered_map<std::string, std::string> headers_; ///< 响应头
    std::string body_;              ///< 用于保存普通文本内容

    std::string filePath_;          ///< 用于保存要发送的文件路径
    size_t fileSize_;               ///< 要发送的文件大小 (字节)
    bool isFile_;                   ///< 标识当前响应是否为发送文件

public:
    // 默认构造一个成功的 200 响应
    HttpResponse() : version_("HTTP/1.1"), statusCode_(200), statusMessage_("OK"), fileSize_(0), isFile_(false) {
        // 默认加上长连接关闭的请求头，避免浏览器转圈
        headers_["Connection"] = "keep-alive"; // 设置为长连接 
    }
    ~HttpResponse() = default;

    // --- 设置状态和内容 ---
    void setStatusCode(int code, const std::string& msg) {
        statusCode_ = code;
        statusMessage_ = msg;
    }
    
    void addHeader(const std::string& key, const std::string& value) {
        headers_[key] = value;
    }
    
    void setBody(const std::string& body) {
        body_ = body;
        isFile_ = false; // 若设置了普通文本，确保取消文件标记
        // 极其智能的一步：只要设置了 Body，自动计算并加上 Content-Length！
        // 彻底杜绝上次浏览器无限转圈的惨剧！
        headers_["Content-Length"] = std::to_string(body_.size());
    }

    /**
     * @brief 设置 Cookie
     * @param key Cookie 的键
     * @param val Cookie 的值
     * @param Max_age Cookie 的最大存活时间
     */
    void set_cookie(const std::string& key, const std::string& val, int Max_age) {
        headers_["Set-Cookie"] = key + "=" + val + "; Max-Age=" + std::to_string(Max_age);
    }

    // --- 核心序列化功能 ---
    /**
     * @brief 将当前的 C++ 对象，打包成符合 HTTP 协议的纯文本字符串，准备发给底层 TCP
     */
    std::string assemble() const {
        std::string response;
        // 1. 拼装响应行
        response += version_ + " " + std::to_string(statusCode_) + " " + statusMessage_ + "\r\n";
        
        // 2. 拼装响应头
        for (const auto& header : headers_) {
            response += header.first + ": " + header.second + "\r\n";
        }
        
        // 3. 拼装空行 (极其致命的分界线)
        response += "\r\n";
        
        // 4. 拼装响应体
        response += body_;
        
        return response;
    }

    /**
     * @brief 业务层调用此方法，告诉 Response 需要发送一个本地文件
     * @param filepath 文件的路径
     * @return 如果文件存在且可读，返回 true；如果文件不存在，返回 false (业务层收到 false 可以立刻改发 404)
     */
    bool setFile(const std::string& filepath) {
        /**
         * @brief 用于存储文件状态信息的结构体 (系统级结构体)
         * @details 该结构体由 stat() 系统调用填充，主要包含以下核心成员：
         *          - st_dev:   包含该文件的设备 ID
         *          - st_ino:   文件的 inode 节点号
         *          - st_mode:  文件的类型和权限标志 (如是否为普通文件、目录，以及读写执行权限)
         *          - st_nlink: 指向该文件的硬链接数
         *          - st_uid:   文件所有者的用户 ID
         *          - st_gid:   文件所有者的组 ID
         *          - st_size:  ★ 文件的精确总字节大小 (本项目中主要提取此字段用于构造 HTTP 的 Content-Length)
         *          - st_atime: 文件的最后访问时间 (Access time)
         *          - st_mtime: 文件的最后修改时间 (Modification time，指文件内容被修改的时间)
         *          - st_ctime: 文件的最后状态变更时间 (Change time，指文件权限或属性等元数据被修改的时间)
         */
        struct stat file_stat;

        /**
         * @brief 获取指定路径文件的状态信息 (系统调用)
         * @signature int stat(const char *pathname, struct stat *statbuf);
         * @param pathname 指向要获取状态信息的文件路径的 C 风格字符串
         * @param statbuf  传出参数，指向用于保存文件状态信息的 struct stat 缓冲区的指针
         * @return 成功执行返回 0，同时将文件详细信息填入 statbuf 中；失败返回 -1 并设置 errno (如文件不存在或无权限)
         */
        if (stat(filepath.c_str(), &file_stat) == 0) {
            filePath_ = filepath;
            fileSize_ = file_stat.st_size;
            headers_["Content-Length"] = std::to_string(fileSize_);
            isFile_ = true; // 成功读取到文件信息后，打上文件发送标记
            return true;
        }
        
        LOG_ERROR << "文件 " << filepath << " 不存在或不可读!";
        return false;
    }

    std::string getFilePath() const { return filePath_; }
    size_t getFileSize() const { return fileSize_; }
    bool isFile() const { return isFile_; }

    /**
     * @brief 序列化：现在只拼装 HTTP 头部了！(极其重要)
     * @details 如果带有 body_，把它拼在头部后面；如果是文件，绝不在这里读文件，只返回头部字符串，文件留给底层的 TcpConnection 用 sendfile 去发！
     */
    std::string assembleHeaders() const {
        std::string response;
        response += version_ + " " + std::to_string(statusCode_) + " " + statusMessage_ + "\r\n";
        for (const auto& header : headers_) {
            response += header.first + ": " + header.second + "\r\n";
        }
        response += "\r\n";
        
        // 如果有普通的短文本 body_，还是可以顺手拼上的
        if (!body_.empty()) {
            response += body_;
        }
        
        return response;
    }
};

} // namespace MyServer