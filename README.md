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
- **HTTP 连接池**：懒创建 + LIFO 复用 + 空闲超时检测，按 EventLoop 分桶管理，RAII 自动归还
- **日志级别过滤**：支持 INFO/WARNING/ERROR/FATAL 四级过滤，通过配置文件控制，宏级别零开销拦截
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
│   ├── ConnectionPool.hpp       # HTTP 连接池（单例对象池）
│   ├── ConnectPoolRAII.hpp      # 连接池 RAII 守卫
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
│   ├── ConnectionPool.cpp       # 连接池实现
│   ├── InetAddress.cpp          # DNS 解析 (getaddrinfo)
│   └── ...
└── third_party/
    └── json.hpp                 # nlohmann/json 库
```

## 快速开始（本地部署）

> 适用于在本机（WSL / 原生 Linux）运行网关，Claude Code 通过 `127.0.0.1` 连接。

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
        "thread_num": 4,
        "log_level": "WARNING"
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
| `server.log_level` | 日志级别：`INFO`（全部）/ `WARNING` / `ERROR` / `FATAL` |
| `backend.host` | 后端 API 域名 |
| `backend.path` | API 请求路径 |
| `backend.api_key` | API 密钥 |
| `backend.target_model` | 目标模型名称 |

### 4. 生成前端 SSL 证书（首次必需）

网关前端使用 HTTPS，首次需生成包含 **SAN**（Subject Alternative Name）扩展字段的自签名证书。普通的 CN-only 证书在现代 TLS 客户端中会报 hostname mismatch。

```bash
cd OmniGateway
mkdir -p certs
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

### 6. 配置 Claude Code 连接网关

网关使用 HTTPS，需要让 Claude Code 信任自签名证书。由于 **Node.js 不读取系统 CA 库**（`update-ca-certificates` 对 Node.js 无效），需通过 `NODE_EXTRA_CA_CERTS` 追加信任：

```bash
export ANTHROPIC_API_KEY="any-key-here"
export ANTHROPIC_BASE_URL="https://127.0.0.1:8080"
export NODE_EXTRA_CA_CERTS=/path/to/OmniGateway/certs/server.crt
claude --dangerously-skip-permissions
```

建议将以上三行写入 `~/.bashrc`，避免每次手动 export。

> **说明**：`ANTHROPIC_API_KEY` 可以填任意值（网关不校验），真正的后端密钥在 `gateway_config.json` 中配置。

---

## 云服务器部署

> 适用于将网关部署到具有公网 IP 的云服务器，Claude Code 从本地或其他机器远程连接。

### 1. 服务端：环境准备

在云服务器上安装依赖并克隆项目：

```bash
sudo apt update && sudo apt install -y build-essential cmake libssl-dev git

git clone https://github.com/1774630667qq-alt/OmniGateway.git
cd OmniGateway
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

### 2. 服务端：生成 SSL 证书

证书的 SAN 必须包含服务器的**公网 IP**，否则 Claude Code 连接时会报 hostname mismatch：

```bash
cd OmniGateway
mkdir -p certs
openssl req -x509 -newkey rsa:2048 \
  -keyout certs/server.key \
  -out certs/server.crt \
  -days 3650 -nodes \
  -subj "/CN=你的公网IP" \
  -addext "subjectAltName=IP:你的公网IP"
```

### 3. 服务端：配置网关

编辑 `gateway_config.json`，填入后端 API 信息：

```json
{
    "server": {
        "port": 8080,
        "thread_num": 4,
        "log_level": "WARNING"
    },
    "backend": {
        "host": "api.edgefn.net",
        "path": "/v1/chat/completions",
        "api_key": "sk-your-real-api-key",
        "target_model": "GLM-5"
    }
}
```

### 4. 服务端：开放防火墙端口

网关默认监听 8080 端口，需确保该端口对外可访问：

```bash
# 方式一：ufw（Ubuntu 默认）
sudo ufw allow 8080/tcp

# 方式二：iptables
sudo iptables -A INPUT -p tcp --dport 8080 -j ACCEPT
```

> **重要**：同时需要在云服务商控制台（如阿里云安全组、腾讯云防火墙）中放行 **TCP 8080 入站**规则。

### 5. 服务端：启动网关

```bash
# 前台运行（调试用）
cd build && ./OmniGateway

# 后台运行（生产用）
cd build && nohup ./OmniGateway > /dev/null 2>&1 &

# 或使用 systemd 托管（推荐）
# 参见下方 systemd 配置
```

**可选：创建 systemd 服务实现开机自启：**

```bash
sudo tee /etc/systemd/system/omnigateway.service << 'EOF'
[Unit]
Description=OmniGateway Protocol Translation Gateway
After=network.target

[Service]
Type=simple
WorkingDirectory=/path/to/OmniGateway/build
ExecStart=/path/to/OmniGateway/build/OmniGateway
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now omnigateway
```

### 6. 客户端：下载证书并配置 Claude Code

在你的**本地机器**（运行 Claude Code 的地方）执行以下操作。根据你的操作系统选择对应的指令：

#### 第一步：从云服务器下载证书到本地

<details>
<summary><b>Linux / macOS / WSL</b></summary>

```bash
scp root@你的公网IP:~/OmniGateway/certs/server.crt ~/omnigateway.crt
```

</details>

<details>
<summary><b>Windows (PowerShell)</b></summary>

```powershell
scp root@你的公网IP:~/OmniGateway/certs/server.crt $env:USERPROFILE\omnigateway.crt
```

</details>

> **说明**：`scp` 使用 `root@` 用户名。如果你在服务器上创建了其他用户，请替换为对应的用户名。

#### 第二步：配置环境变量

<details>
<summary><b>Linux / WSL (Bash)</b></summary>

```bash
export ANTHROPIC_API_KEY="any-key-here"
export ANTHROPIC_BASE_URL="https://你的公网IP:8080"
export NODE_EXTRA_CA_CERTS=~/omnigateway.crt
```

建议写入 `~/.bashrc` 实现持久化：

```bash
echo 'export ANTHROPIC_API_KEY="any-key-here"' >> ~/.bashrc
echo 'export ANTHROPIC_BASE_URL="https://你的公网IP:8080"' >> ~/.bashrc
echo 'export NODE_EXTRA_CA_CERTS=~/omnigateway.crt' >> ~/.bashrc
source ~/.bashrc
```

</details>

<details>
<summary><b>macOS (Zsh)</b></summary>

```zsh
export ANTHROPIC_API_KEY="any-key-here"
export ANTHROPIC_BASE_URL="https://你的公网IP:8080"
export NODE_EXTRA_CA_CERTS=~/omnigateway.crt
```

建议写入 `~/.zshrc` 实现持久化：

```zsh
echo 'export ANTHROPIC_API_KEY="any-key-here"' >> ~/.zshrc
echo 'export ANTHROPIC_BASE_URL="https://你的公网IP:8080"' >> ~/.zshrc
echo 'export NODE_EXTRA_CA_CERTS=~/omnigateway.crt' >> ~/.zshrc
source ~/.zshrc
```

</details>

<details>
<summary><b>Windows (PowerShell) — 临时设置（当前会话有效）</b></summary>

```powershell
$env:ANTHROPIC_API_KEY = "any-key-here"
$env:ANTHROPIC_BASE_URL = "https://你的公网IP:8080"
$env:NODE_EXTRA_CA_CERTS = "$env:USERPROFILE\omnigateway.crt"
```

</details>

<details>
<summary><b>Windows (PowerShell) — 永久设置（写入用户环境变量）</b></summary>

```powershell
[System.Environment]::SetEnvironmentVariable("ANTHROPIC_API_KEY", "any-key-here", "User")
[System.Environment]::SetEnvironmentVariable("ANTHROPIC_BASE_URL", "https://你的公网IP:8080", "User")
[System.Environment]::SetEnvironmentVariable("NODE_EXTRA_CA_CERTS", "$env:USERPROFILE\omnigateway.crt", "User")
```

> **注意**：永久设置后需要**重启 PowerShell** 窗口才能生效。

</details>

<details>
<summary><b>Windows (CMD)</b></summary>

```cmd
set ANTHROPIC_API_KEY=any-key-here
set ANTHROPIC_BASE_URL=https://你的公网IP:8080
set NODE_EXTRA_CA_CERTS=%USERPROFILE%\omnigateway.crt
```

如需永久设置，使用 `setx` 命令：

```cmd
setx ANTHROPIC_API_KEY "any-key-here"
setx ANTHROPIC_BASE_URL "https://你的公网IP:8080"
setx NODE_EXTRA_CA_CERTS "%USERPROFILE%\omnigateway.crt"
```

</details>

> **说明**：`ANTHROPIC_API_KEY` 可以填任意值（网关不校验），真正的后端密钥在服务端 `gateway_config.json` 中配置。

#### 第三步：启动 Claude Code

所有平台通用：

```bash
claude --dangerously-skip-permissions
```

> 在 Windows 上，建议在 **WSL** 或 **Git Bash** 中运行 Claude Code。如果在 PowerShell 中运行，请确保 `claude` 已加入 PATH。

### 7. 验证部署

在本地终端测试连通性：

<details>
<summary><b>Linux / macOS / WSL</b></summary>

```bash
# 测试 TCP 端口是否可达
nc -zv 你的公网IP 8080

# 测试 HTTPS 是否正常（忽略证书校验）
curl -k https://你的公网IP:8080/api/hello
# 期望返回：{"status":"ok"}
```

</details>

<details>
<summary><b>Windows (PowerShell)</b></summary>

```powershell
# 测试 TCP 端口是否可达
Test-NetConnection -ComputerName 你的公网IP -Port 8080

# 测试 HTTPS 是否正常（忽略证书校验）
# PowerShell 7+
Invoke-WebRequest -Uri "https://你的公网IP:8080/api/hello" -SkipCertificateCheck
# PowerShell 5.x（需先跳过证书检查）
[System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
Invoke-WebRequest -Uri "https://你的公网IP:8080/api/hello"
```

</details>

<details>
<summary><b>Windows (CMD)</b></summary>

```cmd
# 测试 TCP 端口是否可达（需安装 curl）
curl -k https://你的公网IP:8080/api/hello
```

> Windows 10/11 自带 `curl.exe`，可在 CMD 中直接使用。但在 PowerShell 中 `curl` 是 `Invoke-WebRequest` 的别名，参数不同，请注意区分。

</details>

### 本地 vs 云服务器对比

| 项目 | 本地部署 | 云服务器部署 |
|-----|---------|------------|
| 代码修改 | 无 | 无 |
| 监听地址 | `0.0.0.0:8080` | `0.0.0.0:8080` |
| SSL 证书 SAN | `IP:127.0.0.1` | `IP:公网IP` |
| `ANTHROPIC_BASE_URL` | `https://127.0.0.1:8080` | `https://公网IP:8080` |
| 证书分发 | 本地直接引用 | 需 scp 到客户端 |
| 防火墙 | 无需配置 | 需放行端口 |
| 后端 API 连接 | 可能需要代理 | 通常可直连 |

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
