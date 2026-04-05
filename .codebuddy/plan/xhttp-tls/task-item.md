# xhttp TLS 支持 — 实施计划

- [ ] 1. 定义 Transport vtable 接口与 Plain TCP 实现
   - 新建 `xhttp/transport.h`，定义 `xHttpTransport` vtable 结构体，包含 `read`、`writev`、`handshake`、`alpn`、`destroy` 函数指针
   - 新建 `xhttp/transport_plain.c`，实现 Plain TCP transport：`read` 调用 `read(2)`，`writev` 调用 `writev(2)`，`handshake`/`alpn` 为 NULL，`destroy` 为空操作
   - 在 `xHttpConn_`（`server_private.h`）中新增 `xHttpTransport transport` 字段
   - _需求：1.1, 1.2, 1.4, 1.5_

- [ ] 2. 改造 server.c I/O 路径使用 Transport vtable
   - 修改 `on_listen_event` 中 accept 后的连接初始化，使用 Plain TCP transport 初始化 `conn->transport`
   - 修改 `on_conn_event` 中的读路径：将 `xIOBufferReadFd(&conn->read_buf, xSocketFd(conn->sock))` 替换为通过 `conn->transport.read` 调用
   - 修改 `conn_try_flush` 中的写路径：将 `xIOBufferWriteFd(&conn->write_buf, xSocketFd(conn->sock))` 替换为通过 `conn->transport.writev` 调用
   - 在 `on_conn_event` 中增加握手阶段：若 `conn->transport.handshake` 非 NULL 且握手未完成，先执行握手；若为 NULL 则跳过直接进入协议检测
   - 确保全量单测通过，行为与改造前完全一致
   - _需求：1.3, 1.4, 1.5_

- [ ] 3. 泛化 xIOBuffer I/O 接口支持自定义读写函数
   - 在 `xbuf/io.h` 中新增 `xIOBufferReadFunc` / `xIOBufferWritevFunc` 函数指针类型定义
   - 新增 `xIOBufferReadWith(xIOBuffer *io, xIOBufferReadFunc fn, void *ctx)` 和 `xIOBufferWriteWith(xIOBuffer *io, xIOBufferWritevFunc fn, void *ctx)` 函数
   - 在 `xbuf/io.c` 中实现这两个函数，逻辑与 `xIOBufferReadFd`/`xIOBufferWriteFd` 相同，但底层 I/O 调用替换为用户传入的函数指针
   - 保持 `xIOBufferReadFd`/`xIOBufferWriteFd` 不变（向后兼容）
   - 为新函数编写单元测试
   - _需求：7.1, 7.2, 7.3_

- [ ] 4. 适配协议检测逻辑支持 ALPN
   - 修改 `on_conn_event` 中的协议检测分支：若 `conn->transport.alpn` 非 NULL，调用 `alpn()` 获取协商结果，根据 "h2" 初始化 H2、"http/1.1" 或空结果初始化 H1
   - 若 `conn->transport.alpn` 为 NULL（Plain TCP），保持现有的 H2 magic 首字节检测逻辑不变
   - _需求：8.1, 8.2, 8.3_

- [ ] 5. 新增 FindMbedTLS.cmake 模块
   - 在 `cmake/` 目录下新建 `FindMbedTLS.cmake`，参照 `FindNghttp2.cmake` 的风格
   - 搜索 `mbedtls/ssl.h` 头文件和 `mbedtls`、`mbedcrypto`、`mbedx509` 三个库
   - 创建 `MbedTLS::MbedTLS` imported target，设置 `INTERFACE_INCLUDE_DIRECTORIES` 和 `INTERFACE_LINK_LIBRARIES`
   - _需求：3.1, 3.2, 3.3_

- [ ] 6. 改造 CMakeLists.txt 支持编译时 TLS 库自动检测
   - 在 `xhttp/CMakeLists.txt` 中新增 `XK_TLS_BACKEND` 选项（STRING 类型，默认 "auto"）
   - 实现 auto 模式：先 `find_package(OpenSSL QUIET)`，未找到则 `find_package(MbedTLS QUIET)`
   - 实现手动指定模式：`openssl` 用 `REQUIRED`，`mbedtls` 用 `REQUIRED`，`none` 跳过检测
   - 根据检测结果定义 `XK_HAS_OPENSSL` 或 `XK_HAS_MBEDTLS` 宏，条件编译对应的 `transport_tls_openssl.c` 或 `transport_tls_mbedtls.c`
   - 输出 `message(STATUS ...)` 显示所选 TLS 后端
   - 确保无 TLS 库时编译不报错
   - _需求：2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8_

- [ ] 7. 定义 xHttpServerListenTLS 公共 API 与 stub 实现
   - 在 `xhttp/server.h` 中新增 `xHttpTlsConfig` 结构体（`cert_file`、`key_file`、`ca_file`、`verify_client`）和 `xHttpServerListenTLS` 函数声明
   - 在 `xhttp/server.c` 中新增 stub 实现：当 `XK_HAS_OPENSSL` 和 `XK_HAS_MBEDTLS` 均未定义时，返回 `xErrno_NotSupported`
   - 实现参数校验：`cert_file`/`key_file` 为 NULL 时返回 `xErrno_InvalidArg`
   - 在 `xHttpServer_` 中新增 TLS 上下文指针字段（`void *tls_ctx`），以及 TLS 监听 socket 相关字段
   - _需求：6.1, 6.2, 6.3, 6.4, 6.6_

- [ ] 8. 实现 OpenSSL TLS Transport 后端
   - 新建 `xhttp/transport_tls_openssl.c`，用 `#ifdef XK_HAS_OPENSSL` 包裹
   - 实现 `xHttpTlsCtxCreateOpenSSL`：创建 `SSL_CTX`，加载证书/私钥，配置 ALPN 回调（"h2", "http/1.1"）
   - 实现 TLS transport vtable：`read` 用 `SSL_read`，`writev` 用 `SSL_write`（循环写入各 iovec），`handshake` 用 `SSL_do_handshake`（处理 `WANT_READ`/`WANT_WRITE` 返回对应事件掩码），`alpn` 用 `SSL_get0_alpn_selected`，`destroy` 释放 `SSL` 对象
   - 在 `xHttpServerListenTLS` 中（OpenSSL 分支）创建 TLS 上下文，accept 后为每个连接创建 `SSL` 对象并绑定 fd
   - 在 `on_conn_event` 握手阶段处理异步 TLS 握手，握手完成后通过 ALPN 结果初始化协议
   - _需求：4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 4.8, 4.9, 4.10_

- [ ] 9. 实现 mbedTLS Transport 后端
   - 新建 `xhttp/transport_tls_mbedtls.c`，用 `#ifdef XK_HAS_MBEDTLS` 包裹
   - 实现 `xHttpTlsCtxCreateMbedTLS`：初始化 `mbedtls_ssl_config`，加载证书/私钥，配置 ALPN
   - 实现 TLS transport vtable：`read` 用 `mbedtls_ssl_read`，`writev` 用 `mbedtls_ssl_write`，`handshake` 用 `mbedtls_ssl_handshake`（处理 `WANT_READ`/`WANT_WRITE`），`alpn` 用 `mbedtls_ssl_get_alpn_protocol`，`destroy` 释放 `mbedtls_ssl_context`
   - 确保与 OpenSSL 后端在功能上完全等价（相同的 API 语义、ALPN 协商逻辑、错误处理）
   - _需求：5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7_

- [ ] 10. 集成测试与验证
   - 编写 TLS 集成测试：使用自签名证书，验证 HTTPS 连接建立、ALPN 协商（H2/H1）、请求/响应完整流程
   - 测试无 TLS 库时 `xHttpServerListenTLS` 返回 `xErrno_NotSupported`
   - 测试参数校验：`cert_file`/`key_file` 为 NULL、文件不存在等边界情况
   - 测试 Plain TCP 连接在 Transport 改造后行为不变（运行现有全量单测）
   - 测试同时监听 HTTP 和 HTTPS 端口的场景
   - _需求：6.3, 6.4, 6.5, 6.6, 1.5_
