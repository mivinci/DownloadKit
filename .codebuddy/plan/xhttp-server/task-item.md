# 实施计划：xhttp/server

- [ ] 1. CMake 构建集成 — 引入 llhttp 并扩展 xhttp 构建
  - 在 `xhttp/CMakeLists.txt` 中添加 `find_package(Llhttp REQUIRED)`，将 `Llhttp::Llhttp` 加入 `target_link_libraries`
  - 确保 `GLOB_RECURSE` 能自动收集新增的 server 源文件（现有 `*.c` / `*.h` 通配已满足）
  - 添加条件编译宏 `XK_HAS_LLHTTP`，与现有 `XK_HAS_CURL` 风格一致
  - _需求：8.1、8.2、8.3、8.4_

- [ ] 2. 定义 server 公开头文件 `xhttp/server.h`
  - 声明 opaque handle `xHttpServer`（`XDEF_HANDLE`）
  - 声明 `xHttpRequest` 结构体（method、url、headers、body、body_len）
  - 声明 `xHttpResponseWriter` opaque handle
  - 声明 handler 回调类型 `typedef void (*xHttpHandlerFunc)(xHttpRequest *req, xHttpResponseWriter writer, void *arg)`
  - 声明生命周期 API：`xHttpServerCreate`、`xHttpServerListen`、`xHttpServerDestroy`
  - 声明路由 API：`xHttpServerRoute`
  - 声明响应 API：`xHttpResponseSetStatus`、`xHttpResponseSetHeader`、`xHttpResponseSend`
  - 声明配置 API：`xHttpServerSetIdleTimeout`、`xHttpServerSetMaxHeaderSize`、`xHttpServerSetMaxBodySize`
  - 保持与 `client.h` 一致的注释风格（Doxygen `@brief` / `@param` / `@return`）
  - _需求：1.1、1.2、1.3、4.1、4.2、5.1–5.4、6.1_

- [ ] 3. 实现 server 内部数据结构 `xhttp/server_private.h`
  - 定义 `xHttpServer` 内部结构：`xEventLoop loop`、监听 `xSocket`、路由链表、配置参数（idle_timeout / max_header_size / max_body_size 及默认值）、活跃连接链表
  - 定义 `xHttpConn` 内部结构：`xSocket sock`、读写 `xIOBuffer`、`llhttp_t` 解析器 + `llhttp_settings_t`、当前 `xHttpRequest` 解析状态、`xHttpResponseWriter` 状态、keep-alive 标志、所属 server 指针、链表节点
  - 定义路由条目结构：method、path、handler、arg、next 指针
  - _需求：2.1、3.1、3.2、4.1_

- [ ] 4. 实现服务器生命周期与监听逻辑 `xhttp/server.c`（第一部分）
  - 实现 `xHttpServerCreate`：分配 server 结构、绑定 event loop、初始化路由链表和连接链表、设置默认配置值
  - 实现 `xHttpServerListen`：创建监听 socket（`SO_REUSEADDR`）、bind、listen、注册到 event loop 的读事件、设置 accept 回调
  - 实现 `xHttpServerDestroy`：关闭监听 socket、遍历活跃连接链表逐一关闭、释放路由链表、释放 server 结构
  - 实现配置接口：`xHttpServerSetIdleTimeout`、`xHttpServerSetMaxHeaderSize`、`xHttpServerSetMaxBodySize`，含参数校验
  - 所有公开 API 添加 NULL 参数检查
  - _需求：1.1–1.5、6.1–6.3、7.1_

- [ ] 5. 实现连接 accept 与连接管理
  - 实现 accept 回调：调用 `accept()`、创建 `xHttpConn`（分配 xSocket + 读写 xIOBuffer + 初始化 llhttp 解析器）、将连接加入 server 活跃连接链表、注册 socket 读事件
  - 处理 `EMFILE`/`ENFILE`：记录警告日志，不崩溃
  - 实现连接关闭函数：从活跃连接链表移除、释放 xIOBuffer、重置 llhttp 解析器、关闭 xSocket、释放 xHttpConn
  - 设置空闲超时：利用 `xSocketSetTimeout` 在 `idle_timeout_ms` 后自动触发关闭
  - 处理客户端主动关闭（read 返回 0）：调用连接关闭函数
  - _需求：2.1–2.6、7.2、7.5_

- [ ] 6. 实现 HTTP 请求解析（llhttp 回调集成）
  - 配置 `llhttp_settings_t` 回调：`on_url`、`on_header_field`、`on_header_value`、`on_headers_complete`、`on_body`、`on_message_complete`
  - 在 socket 可读回调中：使用 `xIOBufferReadFd` 读取数据，将数据块逐一喂给 `llhttp_execute`
  - 在 `on_headers_complete` 中：检查 header 总大小是否超过 `max_header_size`，超限返回 431
  - 在 `on_body` 中：累计 body 大小，超过 `max_body_size` 返回 413
  - 在 `on_message_complete` 中：构造 `xHttpRequest`，提取 method / url / headers / body，触发路由匹配
  - 处理 chunked transfer-encoding（llhttp 自动解码）
  - 处理解析错误：发送 400 Bad Request 并关闭连接
  - _需求：3.1–3.7_

- [ ] 7. 实现路由注册与请求分发
  - 实现 `xHttpServerRoute`：分配路由条目、追加到路由链表尾部
  - 实现路由匹配函数：遍历路由链表，按注册顺序匹配 path（精确匹配）和 method（NULL method 匹配所有）
  - 匹配成功：调用 handler 回调，传入 `xHttpRequest` 和 `xHttpResponseWriter`
  - 路径无匹配：自动返回 404 Not Found
  - 路径匹配但方法不匹配：自动返回 405 Method Not Allowed
  - _需求：4.1–4.6_

- [ ] 8. 实现响应构建与发送
  - 实现 `xHttpResponseWriter` 内部状态：status code（默认 200）、header 链表、已发送标志
  - 实现 `xHttpResponseSetStatus`：设置状态码
  - 实现 `xHttpResponseSetHeader`：追加 header 到链表
  - 实现 `xHttpResponseSend`：序列化状态行 + headers + body 到写 `xIOBuffer`，调用 `xIOBufferWriteFd` 发送
  - 实现背压控制：`writev` 返回 `EAGAIN` 时，保留剩余数据在写 `xIOBuffer`，注册 `xEvent_Write`，可写时继续发送
  - 实现 handler 返回后的兜底逻辑：若未调用任何响应方法，自动发送 200 OK 空 body
  - 响应发送完成后：根据 `Connection` 头和 HTTP 版本决定 keep-alive 或关闭连接
  - _需求：5.1–5.7、2.2、2.3_

- [ ] 9. 编写单元测试 `xhttp/server_test.cpp`
  - 测试服务器创建/销毁生命周期（含 NULL 参数）
  - 测试监听与端口占用错误处理
  - 测试基本 GET 请求：发送请求 → 路由匹配 → handler 被调用 → 响应正确返回
  - 测试 POST 请求（带 body）
  - 测试 404 / 405 自动响应
  - 测试 keep-alive 连接复用（同一连接发送多个请求）
  - 测试超限请求（header 过大 → 431、body 过大 → 413）
  - 测试解析错误（畸形请求 → 400）
  - 测试空闲超时断开
  - 使用 GTest 框架，与现有 `client_test.cpp` 风格一致
  - _需求：1.4、2.2–2.5、3.5–3.7、4.3–4.4、5.5、7.1–7.2_
