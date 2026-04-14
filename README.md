# OmniGateway

OmniGateway 是一个高性能 C++ 协议翻译网关，将 **Claude Code (Anthropic 协议)** 的请求实时转换为 **OpenAI 兼容协议**，再将后端大模型的流式响应翻译回 Anthropic 格式。这使得 Claude Code CLI 可以透明地调用任意 OpenAI 兼容的大模型（如 GLM-5、DeepSeek 等）。

## 架构概览

```
Claude Code CLI ──(HTTPS/TLS)──▶ OmniGateway ──(HTTPS/TLS)──▶ 后端大模型 API
                ◀──(HTTPS/TLS)──              ◀──(HTTPS/TLS)──
```

**核心数据流：**

1. Claude Code 发送 `/v1/messages` 请求（Anthropic 格式）
2. OmniGateway 翻译为 `/v1/chat/completions`（OpenAI 格式）并转发至后端
3. 后端返回 SSE 流，OmniGateway 逐 chunk 翻译回 Anthropic 格式
4. Claude Code 收到标准的 Anthropic SSE 流，无感知差异

## 功能特性

- **双向协议翻译**：请求层 (Anthropic → OpenAI) + 响应层 (OpenAI → Anthropic)
- **流式 SSE 转发**：零缓冲逐 chunk 翻译，低延迟
- **思考文本分离**：将后端 `reasoning_content` 映射为 Claude 的 `thinking` content block
- **工具调用翻译**：Claude `tool_use` ↔ OpenAI `function_calling` 双向转换
- **异步 DNS 解析**：在线程池中执行阻塞 DNS 查询，不阻塞事件循环
- **全链路 TLS 加密**：前端（客户端→网关）和后端（网关→API）均通过 OpenSSL 全程加密
- **JSON 配置文件**：所有参数均通过 `gateway_config.json` 配置，无需重编译
- **Multi-Reactor 线程模型**：主线程 Accept + IO 线程池处理连接

## 项目结构

```
OmniGateway/
├── gateway_config.json          # 网关配置文件
├── CMakeLists.txt               # 构建脚本
├── include/                     # 头文件
│   ├── ConfigManager.hpp        # 配置管理器（单例）
│   ├── ProtocolTranslator.hpp   # 协议翻译器
│   ├── HttpServer.hpp           # 前端 HTTP 服务器
│   ├── HttpClient.hpp           # 后端 HTTP 客户端
│   ├── HttpParser.hpp           # HTTP 报文解析器
│   ├── TcpServer.hpp            # TCP 服务器
│   ├── TcpConnection.hpp        # TCP 连接管理
│   ├── EventLoop.hpp            # 事件循环（Reactor）
│   ├── Connector.hpp            # 主动连接器
│   ├── InetAddress.hpp          # 网络地址（含 DNS 解析）
│   ├── SSLManager.hpp           # OpenSSL 全局管理
│   ├── ThreadPool.hpp           # 通用线程池
│   ├── Buffer.hpp               # 读写缓冲区
│   ├── Channel.hpp              # epoll 事件通道
│   └── ...
├── src/                         # 源文件
│   ├── main.cpp                 # 主入口 + 路由逻辑
│   ├── ProtocolTranslator.cpp   # 协议翻译实现
│   ├── ConfigManager.cpp        # 配置加载实现
│   ├── HttpClient.cpp           # 含异步 DNS + TLS 握手
│   ├── InetAddress.cpp          # DNS 解析 (getaddrinfo)
│   └── ...
└── third_party/
    └── json.hpp                 # nlohmann/json 库
```

## 快速开始

### 1. 环境要求

- Linux (Ubuntu 22.04+)
- GCC 11+ / Clang 14+ (支持 C++17)
- CMake 3.16+
- OpenSSL 开发库

```bash
sudo apt install build-essential cmake libssl-dev
```

### 2. 编译

```bash
cd OmniGateway
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

### 3. 配置

编辑 `gateway_config.json`：

```json
{
    "server": {
        "port": 8080,
        "thread_num": 4
    },
    "backend": {
        "host": "api.edgefn.net",
        "path": "/v1/chat/completions",
        "api_key": "sk-your-api-key-here",
        "target_model": "Model-ID"
    }
}
```

| 字段 | 说明 |
|------|------|
| `server.port` | 网关监听端口 |
| `server.thread_num` | IO 线程数 |
| `backend.host` | 后端 API 域名 |
| `backend.path` | API 请求路径 |
| `backend.api_key` | API 密钥 |
| `backend.target_model` | 目标模型名称 |

### 4. 生成前端 SSL 证书（首次必需）

网关前端使用 HTTPS，首次需生成包含 **SAN**（Subject Alternative Name）扩展字段的自签名证书。普通的 CN-only 证书在现代 TLS 客户端中会报 hostname mismatch。

```bash
cd OmniGateway
openssl req -x509 -newkey rsa:2048 \
  -keyout certs/server.key \
  -out certs/server.crt \
  -days 3650 -nodes \
  -subj "/CN=127.0.0.1" \
  -addext "subjectAltName=IP:127.0.0.1,IP:::1"
```

### 5. 启动网关

```bash
cd build
./OmniGateway
```

### 5. 配置 Claude Code 连接网关

网关使用 HTTPS，需要让 Claude Code 信任自签名证书。由于 **Node.js 不读取系统 CA 库**（`update-ca-certificates` 对 Node.js 无效），需通过 `NODE_EXTRA_CA_CERTS` 追加信任：

```bash
export ANTHROPIC_API_KEY="any-key-here"
export ANTHROPIC_BASE_URL="https://127.0.0.1:8080"
export NODE_EXTRA_CA_CERTS=/home/bazinga/OmniGateway/certs/server.crt
claude --dangerously-skip-permissions
```

建议将以上三行写入 `~/.bashrc`，避免每次手动 export：

```bash
echo 'export ANTHROPIC_API_KEY="any-key-here"' >> ~/.bashrc
echo 'export ANTHROPIC_BASE_URL="https://127.0.0.1:8080"' >> ~/.bashrc
echo 'export NODE_EXTRA_CA_CERTS=/home/bazinga/OmniGateway/certs/server.crt' >> ~/.bashrc
```

> **说明**：`ANTHROPIC_API_KEY` 可以填任意值（网关不校验），真正的后端密钥在 `gateway_config.json` 中配置。

---

## ⚠️ Claude Code CLI 补丁说明

### 问题背景

Claude Code 在正常工作流中，**并非所有 HTTP 请求都走 `ANTHROPIC_BASE_URL`**。CLI 内部存在两套 API 基地址：

1. **`ANTHROPIC_BASE_URL`**（环境变量）：用于 `/v1/messages` 等核心对话请求 — **✅ 会被正确重定向到网关**
2. **`l7().BASE_API_URL`**（内部配置）：用于会话管理、文件上传、远程会话等辅助请求 — **❌ 始终指向 `https://api.anthropic.com`，不受环境变量控制**

当第 2 类请求因网络环境无法到达 Anthropic 官方服务器时（如国内服务器），Claude Code 可能出现**启动卡顿、功能异常**。

### 解决方案

目前暂无优雅的替代方案，需要直接修改 Claude Code 的 CLI 源文件。

**需修改的文件：**

```
/usr/lib/node_modules/@anthropic-ai/claude-code/cli.js
```

**修改方法：**

搜索 `BASE_API_URL` 的定义位置（在 `l7()` 函数或其相关的配置对象中），将硬编码的 `https://api.anthropic.com` 替换为读取环境变量：

```javascript
// 修改前（原始代码，混淆后的变量名可能不同）
BASE_API_URL: "https://api.anthropic.com"

// 修改后
BASE_API_URL: process.env.ANTHROPIC_BASE_URL || "https://api.anthropic.com"
```

> **注意事项：**
> - 该文件是压缩混淆过的单行巨型 JS 文件，建议使用编辑器的查找替换功能
> - 搜索关键字：`api.anthropic.com`，将与 `BASE_API_URL` 相关的硬编码 URL 替换
> - Claude Code 更新后此修改会被覆盖，需重新应用
> - 此修改仅影响辅助功能（会话管理等），核心对话功能不受影响；如果不需要这些辅助功能，也可以不做此修改

### 验证是否需要补丁

如果你的环境满足以下 **任一** 条件，则 **不需要** 此补丁：
- 服务器可以直接访问 `https://api.anthropic.com`
- 已配置代理（HTTP_PROXY / ALL_PROXY）可达 Anthropic 服务器
- 只使用基础对话功能，不使用远程会话、文件上传等高级特性

---

## 协议翻译映射表

### 请求翻译 (Anthropic → OpenAI)

| Anthropic 字段 | OpenAI 字段 |
|---|---|
| `system` (顶级字段) | `messages[0].role = "system"` |
| `messages[].content` (结构化数组) | 展平为文本/tool_calls |
| `tools[].input_schema` | `tools[].function.parameters` |
| 消息中 `tool_use` 块 | `assistant.tool_calls[]` |
| 消息中 `tool_result` 块 | `role: "tool"` 消息 |
| `thinking` 块 | 跳过不传 |

### 响应翻译 (OpenAI SSE → Anthropic SSE)

| OpenAI 字段 | Anthropic 事件 |
|---|---|
| 首个 chunk | `message_start` |
| `delta.reasoning_content` | `thinking` content block + `thinking_delta` |
| `delta.content` | `text` content block + `text_delta` |
| `delta.tool_calls` | `tool_use` content block + `input_json_delta` |
| `finish_reason: "stop"` | `stop_reason: "end_turn"` |
| `finish_reason: "tool_calls"` | `stop_reason: "tool_use"` |
| `data: [DONE]` | `message_stop` |

## 依赖

| 依赖 | 用途 |
|------|------|
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 解析与生成 |
| OpenSSL | TLS 加密通信 |

## License

MIT
