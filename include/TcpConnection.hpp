/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-03-20 15:29:42
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-03-30 12:55:12
 * @FilePath: /ServerPractice/include/TcpConnection.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <functional>
#include <string>
#include <memory>
#include <queue>
#include <sys/types.h> // 提供 off_t 类型
#include "Buffer.hpp"
#include "Timer.hpp"
#include <atomic>

namespace MyServer {

class EventLoop;
class Channel;

/**
 * @brief 原子状态机，枚举类型，用于表示连接的状态
 */
enum StateE { 
    kConnected,     // 正常连接中
    kDisconnecting, // 正在执行处决命令（微秒级过渡态）
    kDisconnected   // 已经彻底死透了
};

/**
 * @brief 发送任务单元
 * @details 统一管理要发送的数据。它可能是一段普通的 HTTP 头部文本，也可能是一个需要零拷贝发送的巨大文件。
 */
struct OutputItem {
    bool is_file;           ///< 标志位：这是一个文件任务，还是普通的字符串任务？
    
    // --- 字符串任务专属 ---
    std::string str_data;   ///< 如果是字符串，数据存在这里
    
    // --- 文件任务专属 ---
    int file_fd;            ///< 如果是文件，存放底层打开的文件描述符
    off_t file_offset;      ///< 当前文件发到哪个字节了
    size_t file_size;       ///< 文件的总大小
    
    // 构造字符串任务
    OutputItem(const std::string& str) 
        : is_file(false), str_data(str), file_fd(-1), file_offset(0), file_size(0) {}
        
    // 构造文件任务
    OutputItem(int fd, size_t size) 
        : is_file(true), file_fd(fd), file_offset(0), file_size(size) {}
};

/**
 * @brief TCP 连接类：代表一个已经建立连接的客户端 (专属酒席)
 * * 它的核心职责是：管理属于这个客户端的 client_fd，以及它专属的 Channel。
 * * 当这个客户端发来消息时，它负责调用 recv() 把数据读出来，
 * * 然后把读到的数据通过回调函数 (messageCallback_) 汇报给大老板 (TcpServer)。
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
private:
    EventLoop* loop_;       ///< 大管家 (所属的事件循环)
    int fd_;                ///< 与客户端通信的专属 fd
    int connId_;            ///< 唯一连接 ID（由 TcpServer 分配，防止 fd 复用导致误删）
    Channel* channel_;      ///< 属于这个 fd 的专属通信管道 (服务员)
    Buffer buffer_;         ///< 读数据的缓冲区
    std::queue<OutputItem> outputQueue_; ///< 严格保序的发送任务队列
    // Buffer writeBuffer_;    ///< 写数据的缓冲区
    std::shared_ptr<Timer> keepAliveTimer_; ///< 专属秒表：如果长时间没重置，它就会引爆！
    std::atomic<StateE> state_; ///< 原子状态机，用于表示连接的状态

    // --- 给上层大老板 (TcpServer) 留的汇报接口 ---
    
    ///< 当收到客人发来的消息时，触发此回调。参数1是当前连接的智能指针，参数2是收到的字符串
    std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)> messageCallback_;
    
    ///< 当客人断开连接时，触发此回调。参数是当前连接的智能指针
    std::function<void(const std::shared_ptr<TcpConnection>&)> closeCallback_;

    /**
     * @brief 定时器引爆时执行的踢人函数
     */
    void handleTimeout();
public:
    /**
     * @brief 构造函数：接管客户端文件描述符，并初始化其专属 Channel
     * @param loop 所在的 EventLoop 实例
     * @param fd 客户端已连接的非阻塞套接字文件描述符
     */
    TcpConnection(EventLoop* loop, int fd);
    
    /**
     * @brief 析构函数：释放内部专属 Channel，并严格调用 close(fd_) 关闭底层 TCP 通信套接字
     */
    ~TcpConnection();

    /**
     * @brief 核心方法：处理套接字可读事件
     * @details 因为使用的是 EPOLLET (边缘触发) 模式，所以在该函数内部，
     * 会在一个死循环中不断调用 recv() 读取数据，直到返回 EAGAIN 或 EWOULDBLOCK 为止。
     * - 读到数据时，将触发 messageCallback_ 向上层抛出。
     * - 遇到 0 字节断开或不可恢复的错误时，将触发 closeCallback_ 并安全地向上层通知注销自己。
     */
    void handleRead(); 

    /**
     * @brief 处理套接字可写事件
     * @details 当底层 TCP 写缓冲区有空闲空间时（由 epoll 触发 EPOLLOUT），该函数会被调用。
     * 它负责将应用层写缓冲区 (writeBuffer_) 中积压的数据推送到内核网络栈中。
     * 如果所有数据都发送完毕，会取消对 EPOLLOUT 事件的监听，防止 CPU 空转。
     */
    void handleWrite();

    // --- 注册回调的 Setter ---
    void setMessageCallback(std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)> cb) {
        messageCallback_ = std::move(cb);
    }
    void setCloseCallback(std::function<void(const std::shared_ptr<TcpConnection>&)> cb) {
        closeCallback_ = std::move(cb);
    }

    /**
     * @brief 发送数据到客户端 (非阻塞异步发送逻辑)
     * @param msg 要发送的字符串数据
     * @details 
     * 1. 如果当前的写缓冲区 (writeBuffer_) 是空的，则直接尝试调用系统的 `send()` 发送数据。
     * 2. 如果一次性没有发完（发生了 EAGAIN），或者写缓冲区里本身就有积压的数据，
     *    就把剩余未发送的数据追加到写缓冲区中，并向 epoll 注册 EPOLLOUT 可写事件。
     *    由后续触发的 handleWrite() 负责继续发送。
     */
    void send(const std::string& msg);

    /**
     * @brief 获取当前连接所绑定的客户端文件描述符
     * @return 客户端的 Socket FD
     */
    int getFd() const { return fd_; }

    /**
     * @brief 设置唯一连接 ID
     */
    void setConnId(int id) { connId_ = id; }

    /**
     * @brief 获取唯一连接 ID
     */
    int getConnId() const { return connId_; }

    /**
     * @brief 获取当前连接所属的事件循环 (EventLoop) 大管家
     * @return 指向所在的 EventLoop 对象的指针
     */
    EventLoop* getLoop() { return loop_; };

    /**
     * @brief 在 IO 线程中完成连接的初始化（注册 epoll 读事件）
     * @details 必须在连接所属的 ioLoop 线程中调用，不可在主线程中直接调用
     */
    void connectEstablished();

    /**
     * @brief 为当前连接续命 (重置秒表)
     */
    void extendLife();

    /**
     * @brief 零拷贝发送文件接口
     * @param filepath 要发送的本地文件绝对或相对路径
     */
    void sendFile(const std::string& filepath);

    /**
     * @brief 强制关闭连接
     * @details 立即触发 closeCallback_ 并注销自己
     */
    void forceClose();
};

} // namespace MyServer