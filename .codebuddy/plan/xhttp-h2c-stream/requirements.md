# 需求文档：xhttp HTTP/2 (h2c) 支持 + Stream 抽象层

## 引言

本功能为 xKit 的 xhttp 模块引入 HTTP/2 cleartext (h2c) 支持，采用 **Prior Knowledge** 模式（客户端直接发送 HTTP/2 connection preface，无需 HTTP/1.1 Upgrade 握手）。

核心架构变更是引入 `xHttpStream_` 结构体，将请求级状态从 `xHttpConn_`（连接级）中分离出来。HTTP/1.1 下每个连接有一个隐式 stream，HTTP/2 下每个连接可以有多个并发 stream。协议检测通过读取连接首字节（24 字节 HTTP/2 magic）自动完成。

**关键约束：**
- 使用 nghttp2 库处理 HTTP/2 帧编解码和流管理
- 公开 API（`xHttpRequest`、`xHttpResponseWriter`、`xHttpHandlerFunc`）零变更
- 现有 HTTP/1.1 功能和全部 63 个测试必须保持通过
- `FindNghttp2.cmake` 已完成（前置工作）

**当前代码现状：**
- `xHttpConn_` 直接持有请求解析状态（`url`、`headers_raw`、`body` 等）和响应写入器（`writer`）
- `xHttpProto` vtable 已存在，包含 `on_data`、`reset`、`destroy`、`method`、`should_keep_alive` 五个方法
- `proto_h1.c` 已将 llhttp 逻辑隔离在 vtable 后面
- 响应序列化（`xHttpResponseSend`、`xHttpResponseWrite`）直接写 HTTP/1.1 文本到 `conn->write_buf`

## 需求

### 需求 1：引入 xHttpStream_ 结构体

**用户故事：** 作为 xhttp 开发者，我希望将请求级状态从连接中分离到独立的 stream 结构体中，以便 HTTP/2 多路复用时每个 stream 可以独立持有自己的请求和响应状态。

#### 验收标准

1. WHEN `xHttpStream_` 结构体被定义 THEN 它 SHALL 包含以下字段：`conn`（反向指针）、`stream_id`（H1 为 0）、`url`、`header_field`、`headers_raw`、`body`、`header_bytes`、`writer`、`request_complete`、`pending_error`、`pending_error_reason`
2. WHEN `xHttpStream_` 被引入 THEN `xHttpConn_` SHALL 移除上述请求级字段，仅保留连接级状态：`server`、`sock`、`read_buf`、`write_buf`、`proto`、`keep_alive`、`writing`、连接链表指针
3. WHEN HTTP/1.1 连接初始化时 THEN `xHttpConn_` SHALL 持有一个指向单个隐式 `xHttpStream_` 的指针（`conn->stream`）
4. WHEN `xHttpResponseWriter_` 被改造 THEN 其 `conn` 字段 SHALL 替换为 `stream` 字段，通过 `writer->stream->conn` 访问连接级资源

### 需求 2：改造 HTTP/1.1 协议处理器适配 stream

**用户故事：** 作为 xhttp 开发者，我希望 `proto_h1.c` 中的 llhttp 回调指向 `stream->xxx` 而非 `conn->xxx`，以便 H1 在新的 stream 架构下正常工作。

#### 验收标准

1. WHEN llhttp 回调（`on_url`、`on_header_field`、`on_header_value`、`on_body`、`on_message_complete`）触发 THEN 它们 SHALL 读写 `conn->stream->url`、`conn->stream->headers_raw` 等 stream 级字段，而非 conn 级字段
2. WHEN `on_headers_complete` 回调触发 THEN `keep_alive` SHALL 仍然写入 `conn->keep_alive`（因为 keep-alive 是连接级属性）
3. WHEN `xHttpProtoH1Init` 被调用 THEN 它 SHALL 创建并初始化 `conn->stream`（`stream_id = 0`）
4. WHEN 所有 H1 改造完成 THEN 现有全部 63 个测试 SHALL 通过，行为无变化

### 需求 3：改造 server.c 核心逻辑适配 stream

**用户故事：** 作为 xhttp 开发者，我希望 `server.c` 中的 dispatch、response、reset 逻辑通过 stream 操作，以便为 H2 多 stream 场景做好准备。

#### 验收标准

1. WHEN `conn_dispatch_request` 被调用 THEN 它 SHALL 从 `conn->stream` 读取请求数据（url、headers_raw、body），而非直接从 conn 读取
2. WHEN `conn_reset_request_state` 被调用 THEN 它 SHALL 重置 `conn->stream` 中的请求级字段和 writer 状态
3. WHEN `xHttpConnClose` 被调用 THEN 它 SHALL 销毁 `conn->stream`（释放 stream 持有的 buffer 和 response header 链表）
4. WHEN `xHttpConnSendError` 被调用 THEN 它 SHALL 通过 `conn->stream->writer` 发送错误响应
5. WHEN 响应 API（`xHttpResponseSend`、`xHttpResponseWrite`、`xHttpResponseEnd`）被调用 THEN 它们 SHALL 通过 `w->stream->conn` 访问 `write_buf`

### 需求 4：扩展 vtable 支持协议特定的响应序列化

**用户故事：** 作为 xhttp 开发者，我希望响应序列化通过 vtable 多态实现，以便 H1 和 H2 可以用各自的格式（文本 vs 帧）发送响应。

#### 验收标准

1. WHEN vtable 被扩展 THEN `xHttpProto` SHALL 新增以下方法：`send_response(stream, status, headers, body, body_len)`、`write_data(stream, data, len)`、`end_stream(stream)`
2. WHEN `method` vtable 方法被改造 THEN 其签名 SHALL 从 `method(conn)` 变为 `method(stream)`
3. WHEN H1 实现新 vtable 方法 THEN `send_response` SHALL 生成 HTTP/1.1 文本格式的状态行 + 头部 + body 写入 `conn->write_buf`
4. WHEN H1 `write_data` 被调用 THEN 它 SHALL 将数据追加到 `conn->write_buf`（与当前 streaming 行为一致）
5. WHEN H1 `end_stream` 被调用 THEN 它 SHALL 标记 stream 完成（与当前 `xHttpResponseEnd` 行为一致）
6. WHEN 现有公开响应 API 被调用 THEN 它们 SHALL 内部委托给 `conn->proto.send_response` / `write_data` / `end_stream`，而非直接写 HTTP/1.1 文本

### 需求 5：实现协议自动检测（Prior Knowledge）

**用户故事：** 作为 xhttp 开发者，我希望服务器能自动检测客户端使用的是 HTTP/1.1 还是 HTTP/2，以便同一端口同时服务两种协议。

#### 验收标准

1. WHEN 连接首次收到数据 THEN 服务器 SHALL 检查前 24 字节是否匹配 HTTP/2 connection preface（`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`）
2. IF 前 24 字节匹配 HTTP/2 magic THEN 服务器 SHALL 调用 `xHttpProtoH2Init(conn)` 初始化 H2 协议处理器
3. IF 前 24 字节不匹配 HTTP/2 magic THEN 服务器 SHALL 调用 `xHttpProtoH1Init(conn)` 初始化 H1 协议处理器（当前行为）
4. IF 首次收到的数据不足 24 字节 THEN 服务器 SHALL 缓冲数据并等待更多数据到达后再做判断
5. WHEN 协议检测完成 THEN 已缓冲的数据 SHALL 被完整传递给选定的协议处理器的 `on_data` 方法

### 需求 6：实现 HTTP/2 协议处理器（proto_h2.c）

**用户故事：** 作为 xhttp 开发者，我希望有一个基于 nghttp2 的 HTTP/2 协议处理器，以便服务器能处理 HTTP/2 请求。

#### 验收标准

1. WHEN `xHttpProtoH2Init(conn)` 被调用 THEN 它 SHALL 创建 `nghttp2_session`（server 模式）并注册所有必要的回调
2. WHEN nghttp2 `on_begin_headers_callback` 触发 THEN 它 SHALL 为新 stream 创建 `xHttpStream_` 实例
3. WHEN nghttp2 `on_header_callback` 触发 THEN 它 SHALL 将伪头（`:method`、`:path`）和普通头写入对应 stream 的字段
4. WHEN nghttp2 `on_data_chunk_recv_callback` 触发 THEN 它 SHALL 将 body 数据写入对应 stream 的 `body` buffer
5. WHEN nghttp2 `on_frame_recv_callback` 触发且帧带有 END_STREAM 标志 THEN 它 SHALL 标记 stream 的 `request_complete = 1` 并触发 dispatch
6. WHEN nghttp2 `on_stream_close_callback` 触发 THEN 它 SHALL 销毁对应的 `xHttpStream_` 实例
7. WHEN H2 `send_response` vtable 方法被调用 THEN 它 SHALL 使用 `nghttp2_submit_response()` 提交响应头和 body
8. WHEN nghttp2 需要发送数据 THEN `send_callback` SHALL 将帧数据写入 `conn->write_buf`
9. WHEN H2 `on_data` vtable 方法被调用 THEN 它 SHALL 将数据传递给 `nghttp2_session_mem_recv()` 并随后调用 `nghttp2_session_send()` 刷新待发送帧
10. WHEN H2 连接销毁 THEN `destroy` 方法 SHALL 释放 `nghttp2_session` 和所有活跃的 `xHttpStream_` 实例

### 需求 7：CMake 集成

**用户故事：** 作为 xhttp 开发者，我希望 nghttp2 依赖被正确集成到构建系统中，以便项目能编译和链接 HTTP/2 支持。

#### 验收标准

1. WHEN `xhttp/CMakeLists.txt` 被更新 THEN 它 SHALL 调用 `find_package(Nghttp2 REQUIRED)` 并链接 `Nghttp2::Nghttp2`
2. WHEN nghttp2 被找到 THEN 构建系统 SHALL 定义 `XK_HAS_NGHTTP2` 编译宏
3. WHEN `proto_h2.c` 被添加 THEN 它 SHALL 被自动包含在 `GLOB_RECURSE` 的源文件列表中
4. WHEN 构建完成 THEN 所有现有测试和新增 H2 测试 SHALL 通过

### 需求 8：测试

**用户故事：** 作为 xhttp 开发者，我希望有充分的测试覆盖 stream 抽象和 HTTP/2 功能，以便确保重构和新功能的正确性。

#### 验收标准

1. WHEN stream 抽象重构完成（需求 1-3）THEN 现有全部 63 个 H1 测试 SHALL 通过，无任何修改
2. WHEN vtable 扩展完成（需求 4）THEN 现有测试 SHALL 通过，响应行为不变
3. WHEN H2 协议处理器完成 THEN 新增测试 SHALL 覆盖：Prior Knowledge 协议检测、单 stream H2 请求/响应、多 stream 并发请求、H2 stream 错误处理、H2 GOAWAY 优雅关闭
4. WHEN H1 和 H2 共存 THEN 测试 SHALL 验证同一端口可以同时接受 H1 和 H2 连接
