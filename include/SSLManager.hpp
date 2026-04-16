/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-08 20:30:59
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-09 15:49:17
 * @FilePath: /OmniGateway/include/SSLManager.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include <openssl/err.h>
#include <openssl/ssl.h>
#include "Logger.hpp"

class SSLManager {
public:
    static void init() {
        /**
         * @brief 初始化 SSL 库
         * @signature int SSL_library_init(void)
         * @details 这个函数必须在任何其他 OpenSSL 函数被调用之前调用。它会注册所有可用的 SSL/TLS 加密算法（密码套件）和摘要算法。
         *          在 OpenSSL 1.1.0 及之后的版本中，初始化是自动的，此函数被保留为宏以保持向后兼容性。
         *          生命周期：应用程序生命周期内仅需调用一次。
         * @return 始终返回 1。
         */
        SSL_library_init();

        /**
         * @brief 将所有的摘要算法和密码算法加载到内存的内部表中
         * @signature void OpenSSL_add_all_algorithms(void)
         * @details 配合 EVP 层（高层密码学构建块）使用，使得可通过算法名称来查找加密或摘要方法。
         *          工作流程：在 SSL 初始化阶段调用，确保后续的证书加载、握手加密等操作能正确找到对应的实现模块。
         */
        OpenSSL_add_all_algorithms();

        /**
         * @brief 加载 SSL 和 Crypto 库的所有错误消息字符串
         * @signature void SSL_load_error_strings(void)
         * @details 职责：使得遇到 SSL 层错误时（比如证书无效、解密失败），可以通过 OpenSSL 提供的工具打印出具有人类可读性的字符串文本，而不是一串晦涩的数字错误码。
         *          工作流程：通常紧跟着 SSL 初始化函数之后被调用，用于增强后续程序的日志可读性。
         */
        SSL_load_error_strings();

        // 1. 初始化 Server 模具
        /**
         * @brief 获取用于服务器的通用 SSL/TLS 方法，并创建新的 SSL 上下文 (Context)
         * @signature const SSL_METHOD *TLS_server_method(void)
         * @signature SSL_CTX *SSL_CTX_new(const SSL_METHOD *method)
         * @param method 选择的 SSL/TLS 协议方法 (这里传入 TLS_server_method() 表示作为服务端并支持自动协商协议最高版本)
         * @details 职责：SSL_CTX 即 `SSL Context`，是 OpenSSL 的核心，充当连接配置的“工厂”对象。保存了配置参数（如 TLS 版本规范、密码套件）以及公共资源（如证书、私钥、会话缓存）。
         *          生命周期：SSL_CTX 与服务器的生命周期相同。不应为每个活动连接重新建立，应当复用全局配置去派生独立的 SSL 连接对象。
         * @return 成功返回有效的 SSL_CTX 指针，失败返回 nullptr。
         */
        serverCtx_ = SSL_CTX_new(TLS_server_method());

        // 加载公钥证书 (crt)
        /**
         * @brief 将本地证书文件加载到 SSL_CTX 中
         * @signature int SSL_CTX_use_certificate_file(SSL_CTX *ctx, const char *file, int type)
         * @param ctx 目标 SSL 上下文对象
         * @param file 证书文件的有效路径 (此处为 PROJECT_ROOT "/certs/server.crt")
         * @param type 证书文件的格式类型 (SSL_FILETYPE_PEM 对应 Base64 文本编码的 PEM 格式)
         * @details 职责：配置服务端的公开身份证明。在建立 TLS 握手阶段，服务器会将该证书发送给客户端以供对方进行验证。证书包含服务端的公钥信息。
         *          工作流程：作为服务器，必须向发起通信的连接对方提供有效 X.509 信任凭证。
         * @return 成功时返回 1；失败则返回 0 或负数。
         */
        if (SSL_CTX_use_certificate_file(serverCtx_, PROJECT_ROOT "/certs/server.crt", SSL_FILETYPE_PEM) <= 0) {
            LOG_FATAL << "无法加载 server.crt";
        }

        // 加载私钥 (key)
        /**
         * @brief 将本地私钥文件加载到 SSL_CTX 中
         * @signature int SSL_CTX_use_PrivateKey_file(SSL_CTX *ctx, const char *file, int type)
         * @param ctx 目标 SSL 上下文对象
         * @param file 包含私钥的文件路径 (此处为 PROJECT_ROOT "/certs/server.key")
         * @param type 私钥文件的解码格式类型 (这里同样为 SSL_FILETYPE_PEM)
         * @details 职责：服务端在握手认证阶段，为了证明自己真实拥有发送给对方那张证书的所有权，需要用这份私钥对加密签名进行操作。
         *          生命周期：一旦成功加载至 SSL_CTX 后就被内部进程接管，应当严格防御其在内存中的泄漏风险。
         * @return 成功返回 1；失败则返回 0 或负数。
         */
        if (SSL_CTX_use_PrivateKey_file(serverCtx_, PROJECT_ROOT "/certs/server.key", SSL_FILETYPE_PEM) <= 0) {
            LOG_FATAL << "无法加载 server.key";
        }

        // 极其重要：验证私钥和证书是否是一对！
        /**
         * @brief 验证 SSL_CTX 中的私钥是否与已加载的证书正确匹配
         * @signature int SSL_CTX_check_private_key(const SSL_CTX *ctx)
         * @param ctx 目标 SSL 上下文对象
         * @details 职责：一个强制安全验证机制。保证当前使用的证书与私钥呈紧密配对的安全形态。
         *          工作流程：对比两者的公钥模数计算是否相同等方式。若加载了相互错乱的文件，运行时客户端强行发生连接握手会导致连接异常截断。
         * @return 两者顺利匹配协同工作返回 1；不匹配或上下文有内部信息缺失则返回 0。
         */
        if (!SSL_CTX_check_private_key(serverCtx_)) {
            LOG_FATAL << "私钥和证书不匹配！";
        }

        /**
         * @brief 设置 SSL 选项，禁用 TLS 重协商（Renegotiation）
         * @details
         *   [问题背景]
         *   在非阻塞事件驱动模型中，当外部客户端通过网络连接时，OpenSSL 可能在
         *   SSL_read() 过程中触发 TLS 重协商（TLS 1.2）或 KeyUpdate（TLS 1.3），
         *   导致 SSL_read() 返回 SSL_ERROR_WANT_WRITE。当前的事件循环虽然会注册
         *   EPOLLOUT，但 handleWrite() 只处理 writeBuffer_ 中的应用层数据，
         *   无法正确驱动 OpenSSL 内部的重协商写出需求，从而导致连接死锁。
         *
         *   [解决方案]
         *   通过 SSL_OP_NO_RENEGOTIATION 禁用服务端发起的 TLS 重协商，
         *   从根源上避免 SSL_read() 返回 WANT_WRITE 的场景。
         *   这不影响正常的 TLS 1.3 握手和数据传输。
         *
         *   [适用范围]
         *   SSL_OP_NO_RENEGOTIATION 在 OpenSSL 1.1.1+ 中可用。
         */
        SSL_CTX_set_options(serverCtx_, SSL_OP_NO_RENEGOTIATION);

        // 2. 初始化 Client 模具
        /**
         * @brief 获取客户端视角的通用 SSL/TLS 方法，创建负责向外代理转发流量的主上下文模具
         * @signature const SSL_METHOD *TLS_client_method(void)
         * @details 职责：指示创建的 SSL Context 模型应用“Client 行为标准”，当代理作为外发客户端对其它安全服务发起访问请求时发挥握手职能。
         * @return 返回相应的 SSL 内部通讯函数族，为 SSL_CTX 提供握手规范模式指导。
         */
        clientCtx_ = SSL_CTX_new(TLS_client_method());
        
        /**
         * @brief 为 SSL_CTX 设置 CA 根信任证书在系统下的标准默认验证路径
         * @signature int SSL_CTX_set_default_verify_paths(SSL_CTX *ctx)
         * @param ctx 目标 SSL 上下文对象 (这里应用给 clientCtx_)
         * @details 职责：加载操作系统的底层证书库目录（例如 /etc/ssl/certs 等）内的权威根证书到信任链缓存表中。
         *          工作流程：每当向外执行标准的 HTTPS 远程请求调用时，必然收到来自对方的数字权威凭证，若无对应签发它的根证书在此受信任列表内进行支撑印证，则认定目标服务器身份不可靠并中断连接。
         * @return 执行成功返回 1；获取或设置配置异常失败返回 0。
         */
        SSL_CTX_set_default_verify_paths(clientCtx_);
    }

    static SSL_CTX *getServerCtx() { return serverCtx_; }
    static SSL_CTX *getClientCtx() { return clientCtx_; }

    static void destroy() {
        /**
         * @brief 清空并彻底销毁相关的 SSL_CTX 环境实例以及关联附带层资源
         * @signature void SSL_CTX_free(SSL_CTX *ctx)
         * @param ctx 将被清理、执行内存解绑最终回收销毁周期的对应上下文对象句柄
         * @details 生命周期：设计安排在程序主进程自然退出并终止服务网络连接事件的结束时段内调用一次。
         *          工作流程：所有由 `SSL_CTX_new` 向内存申请的派生实例必须一一映射并对应通过这个进行终结；该操作连同密钥持有量清退及缓冲区清理一并进行。
         */
        SSL_CTX_free(serverCtx_);
        SSL_CTX_free(clientCtx_);
    }

private:
    /**
     * @brief OpenSSL 全局 SSL/TLS 上下文对象 (工厂)
     * @details SSL_CTX 是 OpenSSL 库的核心结构体，它扮演"TLS 配置工厂"的角色：
     *   - 持有服务器的 **证书** (Certificate) 和 **私钥** (Private Key)
     *   - 配置 TLS **协议版本** (如 TLSv1.2, TLSv1.3)
     *   - 管理 **密码套件** (Cipher Suites) 列表
     *   - 存储 **CA 证书链** 用于验证客户端证书 (mTLS 场景)
     *   - 管理 **会话缓存** (Session Cache) 以加速 TLS 握手复用
     *
     * 工作流程：
     *   1. 服务器启动时创建一个 SSL_CTX，加载证书和密钥
     *   2. 每当有新客户端连接时，通过 SSL_new(sslCtx_) 派生出一个独立的 SSL 对象
     *   3. 该 SSL 对象绑定到具体的 socket fd 上，执行 TLS 握手和加密通信
     *   4. SSL_CTX 在整个服务器生命周期内共享，SSL 对象随连接销毁而释放
     *
     * @note 需在析构函数中调用 SSL_CTX_free(sslCtx_) 释放资源
     * @see SSL_CTX_new(), SSL_CTX_use_certificate_file(), SSL_CTX_use_PrivateKey_file()
     */
    static inline SSL_CTX *serverCtx_;
    static inline SSL_CTX *clientCtx_;
};