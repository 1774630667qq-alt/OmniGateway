/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-08 15:56:09
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-08 19:50:03
 * @FilePath: /OmniGateway/src/HttpClient.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "HttpClient.hpp"

namespace MyServer {
HttpClient::HttpClient(EventLoop* loop, const InetAddress& serverAddr, const ApiConfig& apiConfig)
    : loop_(loop),
      connector_(std::make_shared<Connector>(loop, serverAddr)),
      apiConfig_(apiConfig),
      connected_(false) {
    
}
}// namespace MyServer