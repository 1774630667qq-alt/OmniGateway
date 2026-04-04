#include <sys/epoll.h>
#include <sys/socket.h> // 提供 socket 函数及数据结构
#include <netinet/in.h> // 提供 IPv4 的 sockaddr_in 结构体
#include <arpa/inet.h>  // 提供 IP 地址转换函数
#include <unistd.h>     // 提供 close 函数
#include <iostream>
#include <cstring>      // 提供 memset
#include <fcntl.h>      // 提供 fcntl

// 辅助函数：将文件描述符设置为非阻塞模式
void setNonBlocking(int fd) {
    /**
     * @brief 操作文件描述符，控制其属性和行为
     * @signature int fcntl(int fd, int cmd, ...);
     * @param fd 要操作的文件描述符
     * @param cmd 对文件描述符执行的操作命令。
     *            - F_GETFL: 获取文件访问模式(如 O_RDONLY, O_RDWR)和文件状态标志
     *            - F_SETFL: 设置文件状态标志。常见的文件状态标志包括：
     *                       * O_NONBLOCK: 非阻塞模式，读写不阻塞，
     *                          无数据时立即返回 EAGAIN/EWOULDBLOCK（最常配合 epoll 使用）
     *                       * O_APPEND: 追加模式，每次写操作都追加到文件末尾
     *                       * O_ASYNC: 信号驱动 I/O，文件可读写时内核发送 SIGIO 信号
     *                       * O_SYNC: 同步写入，强制等待数据物理写入底层硬件
     *                       （注：F_SETFL 不能修改文件访问模式和文件创建标志，如 O_CREAT）
     * @param ... (可选参数) 依据 cmd 的不同而不同。当使用 F_SETFL 时，传入要设置的新标志
     * @return 成功时的返回值取决于 cmd (F_GETFL 返回当前标志位，F_SETFL 返回 0)，失败返回 -1
     */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int main () {
    // --- 1. 创建监听套接字 ---
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        std::cerr << "Socket 创建失败!" << std::endl;
        return -1;
    }
    std::cout << "Socket 创建成功，fd: " << listen_fd << std::endl;

    // 开启端口复用
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定地址和端口
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "Bind 失败!" << std::endl;
        close(listen_fd);
        return -1;
    }

    // 监听端口
    if (listen(listen_fd, 128) == -1) {
        std::cerr << "Listen 失败!" << std::endl;
        close(listen_fd);
        return -1;
    }
    std::cout << "服务器启动，正在监听 8080 端口..." << std::endl;

    // --- 2. 创建 epoll 实例 ---
    /**
     * @brief 创建一个新的 epoll 实例
     * @signature int epoll_create1(int flags);
     * @param flags 如果为 0，则行为与 epoll_create() 相同。
     *              可以设置为 EPOLL_CLOEXEC，表示在执行 exec 系列系统调用时自动关闭此文件描述符。
     * @return 成功返回一个新的文件描述符(非负整数)，指向 epoll 实例；失败返回 -1。
     */
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        std::cerr << "Epoll 创建失败!" << std::endl;
        close(listen_fd);
        return -1;
    }
    std::cout << "Epoll 创建成功，fd: " << epfd << std::endl;

    // --- 3. 注册监听套接字到 epoll ---
    /**
     * @brief 用于向 epoll_ctl 注册事件和从 epoll_wait 接收就绪事件的结构体
     *   - events: epoll 事件的位掩码。
     *     - EPOLLIN: 关联的文件可用于读取操作。
     *     - EPOLLOUT: 关联的文件可用于写入操作。
     *     - EPOLLET: 将关联的文件设置为边缘触发(Edge Triggered)模式。默认是水平触发(Level Triggered)。
     *     - EPOLLERR: 关联的文件发生错误。
     *     - EPOLLHUP: 关联的文件被挂断。
     *   - data: 用户数据变量，可以存放任何用户想关联到该文件描述符的数据。
     *     - fd: 通常用来存放文件描述符本身，方便事件就绪时直接获取。
     *     - ptr: 可以指向一个自定义的数据结构。
     */
    struct epoll_event event;
    event.events = EPOLLIN; // 关注可读事件 (LT模式)
    event.data.fd = listen_fd;

    /**
     * @brief 控制 epoll 实例，用于添加、修改或删除要监听的文件描述符及其事件
     * @signature int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
     * @param epfd epoll 实例的文件描述符
     * @param op 要执行的操作，可以是：
     *           - EPOLL_CTL_ADD: 注册新的 fd 到 epfd 上。
     *           - EPOLL_CTL_MOD: 修改已经注册的 fd 的监听事件。
     *           - EPOLL_CTL_DEL: 从 epfd 中删除一个 fd。
     * @param fd 要操作的目标文件描述符
     * @param event 指向 epoll_event 结构体的指针，描述了要监听的事件
     * @return 成功返回 0，失败返回 -1。
     */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        std::cerr << "Epoll 添加监听套接字失败!" << std::endl;
        close(listen_fd);
        close(epfd);
        return -1;
    }

    // 创建数组接收就绪事件
    const int MAX_EVENTS = 1024;
    struct epoll_event active_events[MAX_EVENTS];

    // --- 4. 开启事件驱动循环 ---
    while (true) {
        /**
         * @brief 等待 epoll 实例上的 I/O 事件
         * @signature int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
         * @param epfd epoll 实例的文件描述符
         * @param events 用于存放就绪事件的数组
         * @param maxevents events 数组的大小，即本次调用最多能获取的事件数量
         * @param timeout 等待的超时时间（毫秒）。-1 表示无限期阻塞，直到有事件发生；0 表示立即返回，不阻塞。
         * @return 成功返回就绪的文件描述符数量（大于 0）；超时返回 0；失败返回 -1。
         */
        int nfds = epoll_wait(epfd, active_events, MAX_EVENTS, -1);
        if (nfds == -1) {
            std::cerr << "epoll_wait 失败!" << std::endl;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int active_fd = active_events[i].data.fd;

            if (active_fd == listen_fd) {
                // 情况 A：有新客户端连进来了
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_fd == -1) {
                    std::cerr << "Accept 失败!" << std::endl;
                    continue;
                }
                
                // 将新客户端设置为非阻塞
                setNonBlocking(client_fd);

                // 把新客户端注册给 epoll，并开启边缘触发(ET)
                struct epoll_event client_ev;
                client_ev.data.fd = client_fd;
                client_ev.events = EPOLLIN | EPOLLET; // 关注可读事件，并开启边缘触发(ET)
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev) == -1) {
                    std::cerr << "Epoll 添加客户端套接字失败!" << std::endl;
                    close(client_fd);
                } else {
                    char ip_str[INET_ADDRSTRLEN];
                    /**
                     * @brief 将网络字节序的 IP 地址（二进制）转换为点分十进制的字符串格式
                     * @signature const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
                     * @param af 地址族，AF_INET 表示 IPv4，AF_INET6 表示 IPv6
                     * @param src 指向包含网络字节序 IP 地址的结构体的指针（如 &client_addr.sin_addr）
                     * @param dst 指向用于存放转换后字符串的缓冲区的指针
                     * @param size 缓冲区 dst 的大小（IPv4 通常使用 INET_ADDRSTRLEN，值为 16）
                     * @return 成功返回指向 dst 的非空指针，失败返回 NULL
                     */
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
                    std::cout << "新客户端连接: " << ip_str << ":" << ntohs(client_addr.sin_port) << ", fd: " << client_fd << std::endl;
                }

            } else {
                // 情况 B：已连接的客户端有数据可读
                // 因为我们上面设了 ET 模式，所以必须循环读到 EAGAIN 为止
                char buffer[1024];
                while (true) {
                    // 读取时预留 1 个字节给 '\0'
                    ssize_t bytes_read = recv(active_fd, buffer, sizeof(buffer) - 1, 0);
                    if (bytes_read > 0) {
                        buffer[bytes_read] = '\0'; // 收到多少字节，就在末尾手动截断，防止脏数据污染字符串打印
                        // 收到数据，回显给客户端
                        std::cout << "收到来自 fd " << active_fd << " 的消息: " << buffer << std::endl;
                        send(active_fd, buffer, bytes_read, 0);
                    } else if (bytes_read == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // 数据全读完了，退出内层循环
                            break; 
                        } else {
                            // 真的发生错误了
                            std::cerr << "Recv 错误, fd: " << active_fd << std::endl;
                            close(active_fd); // close 会自动从 epoll 中移除
                            break;
                        }
                    } else if (bytes_read == 0) {
                        // 客户端断开连接
                        std::cout << "客户端断开连接, fd: " << active_fd << std::endl;
                        // close 会自动把该 fd 从 epoll 的红黑树上摘除
                        close(active_fd); 
                        break;
                    }
                }
            }
        }
    }

    close(listen_fd);
    close(epfd);
    return 0;
}