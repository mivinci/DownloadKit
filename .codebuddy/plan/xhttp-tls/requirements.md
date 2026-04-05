# xhttp TLS 支持 — 需求文档

## 引言

xhttp 当前已支持 HTTP/1.1（llhttp）和 HTTP/2（nghttp2，h2c 明文模式），但缺少 TLS 加密传输能力。本功能旨在为 xhttp 引入 TLS 支持，使其能够通过 HTTPS 对外提供服务，同时通过 ALPN 协商自动选择 HTTP/1.1 或 HTTP/2 协议。

### 设计原则

- **编译时自动检测**：CMake 自动检测系统上可用的 TLS 库（OpenSSL 优先，mbedTLS 备选），也支持手动指定或禁用
- **Transport 抽象层**：在连接和协议解析之间引入 transport vtable，将 I/O 操作（read/write）从直接的 fd 系统调用解耦
- **优雅降级**：没有 TLS 库时仍可编译，TLS 相关 API 返回 `NotSupported` 错误码
- **零侵入上层**：路由、Handler、ResponseWriter 等上层 API 无需任何修改

### 当前架构关键点

- `xHttpConn_` 中的 I/O 通过 `xIOBufferReadFd` / `xIOBufferWriteFd` 直接操作 fd
- 协议检测通过 peek 首字节判断 H2 magic 来区分 H1/H2
- 已有 `xHttpProto` vtable 抽象协议层（H1/H2），transport 层可采用类似设计风格

---

## 需求

### 需求 1：Transport 抽象层

**用户故事：** 作为 xhttp 的维护者，我希望将连接的 I/O 操作抽象为 transport vtable，以便在不修改上层代码的前提下支持 Plain TCP 和 TLS 两种传输方式。

#### 验收标准

1. WHEN xhttp 编译完成 THEN 系统 SHALL 包含一个 `xHttpTransport` vtable 结构体，定义 `read`、`writev`、`handshake`、`alpn`、`destroy` 等函数指针
2. WHEN 一个新的 Plain TCP 连接被 accept THEN 系统 SHALL 使用 TCP transport 初始化该连接，其 `read`/`writev` 直接调用系统调用 `read(2)`/`writev(2)`
3. WHEN server.c 中的 `on_conn_event` 处理读写事件 THEN 系统 SHALL 通过 transport vtable 的 `read`/`writev` 进行 I/O，而非直接调用 `xIOBufferReadFd`/`xIOBufferWriteFd`
4. IF transport 的 `handshake` 为 NULL（Plain TCP 场景）THEN 系统 SHALL 跳过握手阶段，直接进入协议检测
5. WHEN Plain TCP transport 被使用 THEN 系统 SHALL 保持与当前完全一致的行为（零功能回归）

### 需求 2：CMake 编译时 TLS 库自动检测

**用户故事：** 作为 xhttp 的使用者，我希望构建系统能自动检测系统上可用的 TLS 库并启用 TLS 支持，以便我不需要手动配置依赖。

#### 验收标准

1. WHEN 用户未指定 `XK_TLS_BACKEND` 选项（或设为 `auto`）THEN CMake SHALL 按 OpenSSL → mbedTLS 的优先级自动检测可用的 TLS 库
2. IF OpenSSL 被检测到 THEN CMake SHALL 定义 `XK_HAS_OPENSSL` 宏，编译 `transport_tls_openssl.c`，并链接 `OpenSSL::SSL` 和 `OpenSSL::Crypto`
3. IF OpenSSL 未找到但 mbedTLS 被检测到 THEN CMake SHALL 定义 `XK_HAS_MBEDTLS` 宏，编译 `transport_tls_mbedtls.c`，并链接 mbedTLS 库
4. IF 两者都未找到 THEN CMake SHALL 输出状态信息 "No TLS library found, TLS support disabled"，且编译不报错
5. WHEN 用户设置 `-DXK_TLS_BACKEND=openssl` THEN CMake SHALL 使用 `find_package(OpenSSL REQUIRED)` 强制要求 OpenSSL
6. WHEN 用户设置 `-DXK_TLS_BACKEND=mbedtls` THEN CMake SHALL 使用 `find_package(MbedTLS REQUIRED)` 强制要求 mbedTLS
7. WHEN 用户设置 `-DXK_TLS_BACKEND=none` THEN CMake SHALL 禁用 TLS 支持，不检测任何 TLS 库
8. WHEN CMake 检测完成 THEN 系统 SHALL 通过 `message(STATUS ...)` 输出所选的 TLS 后端信息

### 需求 3：FindMbedTLS.cmake 模块

**用户故事：** 作为 xhttp 的构建系统维护者，我希望有一个 `FindMbedTLS.cmake` 模块来检测 mbedTLS 库，以便 CMake 能自动发现并链接 mbedTLS。

#### 验收标准

1. WHEN `find_package(MbedTLS)` 被调用 THEN 系统 SHALL 搜索 `mbedtls/ssl.h` 头文件和 `mbedtls`、`mbedcrypto`、`mbedx509` 库文件
2. IF mbedTLS 被找到 THEN 系统 SHALL 创建 `MbedTLS::MbedTLS` imported target，包含所有必要的头文件路径和库链接
3. WHEN FindMbedTLS.cmake 执行 THEN 其风格 SHALL 与现有的 `FindNghttp2.cmake` / `FindLlhttp.cmake` 保持一致

### 需求 4：TLS Transport 实现（OpenSSL 后端）

**用户故事：** 作为 xhttp 的使用者，我希望在系统安装了 OpenSSL 的情况下能通过 HTTPS 提供服务，以便保障传输安全。

#### 验收标准

1. WHEN 编译时定义了 `XK_HAS_OPENSSL` THEN 系统 SHALL 提供基于 OpenSSL 的 TLS transport 实现
2. WHEN `xHttpServerListenTLS` 被调用 THEN 系统 SHALL 创建一个 `SSL_CTX`，加载指定的证书和私钥文件，并配置 ALPN 协议列表（"h2" 和 "http/1.1"）
3. WHEN 一个新的 TLS 连接被 accept THEN 系统 SHALL 创建 `SSL` 对象，绑定到连接 fd，并在 event loop 中异步完成 TLS 握手
4. IF TLS 握手过程中 `SSL_do_handshake` 返回 `SSL_ERROR_WANT_READ` 或 `SSL_ERROR_WANT_WRITE` THEN 系统 SHALL 注册对应的事件并在下次事件触发时继续握手，而非阻塞
5. WHEN TLS 握手完成 THEN 系统 SHALL 通过 `SSL_get0_alpn_selected` 获取协商的协议，并据此初始化 H2 或 H1 协议处理器
6. IF ALPN 协商结果为 "h2" THEN 系统 SHALL 初始化 H2 协议处理器
7. IF ALPN 协商结果为 "http/1.1" 或无 ALPN 结果 THEN 系统 SHALL 初始化 H1 协议处理器
8. WHEN TLS transport 的 `read` 被调用 THEN 系统 SHALL 使用 `SSL_read` 读取解密后的数据
9. WHEN TLS transport 的 `writev` 被调用 THEN 系统 SHALL 使用 `SSL_write` 写入待加密的数据
10. IF `SSL_read`/`SSL_write` 返回 `WANT_READ`/`WANT_WRITE` THEN 系统 SHALL 正确处理并注册对应事件，而非报错

### 需求 5：TLS Transport 实现（mbedTLS 后端）

**用户故事：** 作为 xhttp 的使用者，我希望在系统安装了 mbedTLS（而非 OpenSSL）的情况下也能通过 HTTPS 提供服务，以便在嵌入式或轻量级环境中使用。

#### 验收标准

1. WHEN 编译时定义了 `XK_HAS_MBEDTLS` THEN 系统 SHALL 提供基于 mbedTLS 的 TLS transport 实现
2. WHEN `xHttpServerListenTLS` 被调用 THEN 系统 SHALL 初始化 `mbedtls_ssl_config`，加载证书和私钥，并配置 ALPN 协议列表
3. WHEN 一个新的 TLS 连接被 accept THEN 系统 SHALL 创建 `mbedtls_ssl_context`，绑定到连接 fd，并在 event loop 中异步完成握手
4. IF 握手过程中 `mbedtls_ssl_handshake` 返回 `MBEDTLS_ERR_SSL_WANT_READ` 或 `MBEDTLS_ERR_SSL_WANT_WRITE` THEN 系统 SHALL 注册对应事件并继续握手
5. WHEN TLS 握手完成 THEN 系统 SHALL 通过 `mbedtls_ssl_get_alpn_protocol` 获取协商的协议
6. WHEN mbedTLS transport 的 `read`/`write` 被调用 THEN 系统 SHALL 使用 `mbedtls_ssl_read`/`mbedtls_ssl_write`
7. WHEN mbedTLS transport 被使用 THEN 其行为 SHALL 与 OpenSSL 后端在功能上完全等价（相同的 API、相同的 ALPN 协商逻辑、相同的错误处理语义）

### 需求 6：xHttpServerListenTLS 公共 API

**用户故事：** 作为 xhttp 的使用者，我希望有一个简洁的 API 来启动 HTTPS 监听，以便我只需提供证书和私钥路径即可开启 TLS。

#### 验收标准

1. WHEN 用户调用 `xHttpServerListenTLS(server, host, port, &tls_config)` THEN 系统 SHALL 创建 TLS 上下文并在指定地址端口上监听 HTTPS 连接
2. WHEN `xHttpTlsConfig` 被使用 THEN 其 SHALL 包含以下字段：`cert_file`（证书路径）、`key_file`（私钥路径）、`ca_file`（可选，CA 证书路径）、`verify_client`（客户端验证模式：0=不验证，1=可选，2=必须）
3. IF 编译时没有 TLS 库（既无 OpenSSL 也无 mbedTLS）THEN `xHttpServerListenTLS` SHALL 仍然存在（stub 实现），但返回 `xErrno_NotSupported`
4. IF `cert_file` 或 `key_file` 为 NULL THEN 系统 SHALL 返回 `xErrno_InvalidArg`
5. IF 证书或私钥文件加载失败 THEN 系统 SHALL 返回适当的错误码
6. WHEN TLS 监听和 Plain 监听同时存在 THEN 系统 SHALL 支持在不同端口上同时提供 HTTP 和 HTTPS 服务（分端口策略）

### 需求 7：xIOBuffer I/O 泛化

**用户故事：** 作为 xhttp 的维护者，我希望 xIOBuffer 支持通过自定义读写函数进行 I/O，以便 transport 层可以将 TLS 的 `SSL_read`/`SSL_write` 接入 xIOBuffer 的缓冲机制。

#### 验收标准

1. WHEN xIOBuffer 被使用 THEN 系统 SHALL 提供 `xIOBufferReadFunc` / `xIOBufferWriteFunc` 类型的函数指针接口，允许用户传入自定义的读写函数替代默认的 `read(2)`/`writev(2)`
2. WHEN 自定义读写函数被传入 THEN `xIOBufferReadFd` 和 `xIOBufferWriteFd` 的语义 SHALL 保持不变（返回值、错误处理），仅底层 I/O 调用被替换
3. WHEN 未传入自定义函数（NULL）THEN 系统 SHALL 回退到默认的 `read(2)`/`writev(2)` 行为，保持向后兼容

### 需求 8：协议检测适配

**用户故事：** 作为 xhttp 的维护者，我希望 TLS 连接通过 ALPN 协商协议而非 peek 首字节，以便正确区分 H1 和 H2。

#### 验收标准

1. WHEN 一个 TLS 连接完成握手 THEN 系统 SHALL 通过 transport 的 `alpn()` 方法获取协商结果，并据此初始化协议处理器，跳过现有的 H2 magic 检测逻辑
2. WHEN 一个 Plain TCP 连接到达 THEN 系统 SHALL 保持现有的 H2 magic 首字节检测逻辑不变
3. IF TLS 连接的 ALPN 结果为空（客户端未发送 ALPN）THEN 系统 SHALL 默认使用 H1 协议处理器

---

## 技术约束与边界条件

1. **端口策略**：采用分端口方案（`Listen` vs `ListenTLS`），不在同一端口上混合 TLS 和 Plain TCP
2. **异步握手**：TLS 握手必须在 event loop 中非阻塞完成，不得阻塞事件循环
3. **条件编译**：所有 TLS 相关代码通过 `XK_HAS_OPENSSL` / `XK_HAS_MBEDTLS` 宏条件编译，两个后端互斥（同一次编译只启用一个）
4. **CMake 兼容性**：OpenSSL 使用 CMake 内置的 `FindOpenSSL` 模块，mbedTLS 使用自定义的 `FindMbedTLS.cmake`
5. **内存管理**：TLS 上下文（`SSL_CTX` / `mbedtls_ssl_config`）为 server 级别共享，每个连接创建独立的 TLS session
6. **错误处理**：TLS 握手失败、证书错误等情况应关闭连接并通过 `xLog` 记录日志
