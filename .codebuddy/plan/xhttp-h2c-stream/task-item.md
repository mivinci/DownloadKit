# 实施计划

- [ ] 1. 定义 `xHttpStream_` 结构体并重构 `xHttpConn_`
   - 在 `server_private.h` 中新增 `struct xHttpStream_`，包含字段：`conn`（反向指针）、`stream_id`（int32_t）、`url`（xBuffer）、`header_field`（xBuffer）、`headers_raw`（xBuffer）、`body`（xBuffer）、`header_bytes`（size_t）、`writer`（xHttpResponseWriter_）、`request_complete`（int）、`pending_error`（int）、`pending_error_reason`（const char*）
   - 从 `xHttpConn_` 中移除上述请求级字段（`url`、`header_field`、`headers_raw`、`body`、`header_bytes`、`writer`、`request_complete`、`pending_error`、`pending_error_reason`），新增 `struct xHttpStream_ *stream` 指针
   - 将 `xHttpResponseWriter_` 的 `conn` 字段改为 `struct xHttpStream_ *stream`
   - 新增 `xHttpStream_` 的创建/销毁辅助函数声明（`xHttpStreamCreate`、`xHttpStreamDestroy`、`xHttpStreamReset`）
   - _需求：1.1、1.2、1.3、1.4_

- [ ] 2. 改造 `proto_h1.c` 中的 llhttp 回调指向 stream
   - 修改 `on_url`：将 `conn->url` 改为 `conn->stream->url`，`conn->header_bytes` 改为 `conn->stream->header_bytes`
   - 修改 `on_header_field`：将 `conn->header_bytes`、`conn->headers_raw`、`conn->header_field` 改为 `conn->stream->` 前缀，`conn->pending_error` 改为 `conn->stream->pending_error`
   - 修改 `on_header_value`：将 `conn->header_bytes`、`conn->headers_raw` 改为 `conn->stream->` 前缀
   - 修改 `on_headers_complete`：`conn->keep_alive` 保持不变（连接级属性）
   - 修改 `on_body`：将 `conn->body` 改为 `conn->stream->body`，`conn->pending_error` 改为 `conn->stream->pending_error`
   - 修改 `on_message_complete`：将 `conn->request_complete` 改为 `conn->stream->request_complete`
   - 修改 `h1_on_data`：检查 `conn->stream->pending_error` 和 `conn->stream->request_complete`
   - 在 `xHttpProtoH1Init` 中调用 `xHttpStreamCreate(conn, 0)` 创建隐式 stream 并赋值给 `conn->stream`
   - _需求：2.1、2.2、2.3_

- [ ] 3. 改造 `server.c` 核心逻辑适配 stream
   - 修改 `on_listen_event`：移除 conn 上的 writer 初始化代码（改由 stream 创建时初始化）
   - 修改 `conn_reset_request_state`：通过 `xHttpStreamReset(conn->stream)` 重置 stream 级字段和 writer 状态
   - 修改 `xHttpConnClose`：调用 `xHttpStreamDestroy(conn->stream)` 释放 stream 资源，移除 conn 上的 buffer/header 释放代码
   - 修改 `conn_dispatch_request`：从 `conn->stream->url`、`conn->stream->headers_raw`、`conn->stream->body` 读取请求数据，handler 传入 `&conn->stream->writer`
   - 修改 `on_conn_event`：将 `conn->pending_error` 改为 `conn->stream->pending_error`，`conn->request_complete` 改为 `conn->stream->request_complete`
   - 修改 `xHttpConnSendError`：通过 `conn->stream->writer` 发送错误响应
   - 修改所有响应 API（`xHttpResponseSend`、`xHttpResponseWrite`、`xHttpResponseEnd`、`conn_flush_stream_headers`）：将 `w->conn` 改为 `w->stream->conn`
   - 实现 `xHttpStreamCreate`、`xHttpStreamDestroy`、`xHttpStreamReset` 函数
   - _需求：3.1、3.2、3.3、3.4、3.5_

- [ ] 4. 运行现有测试验证 stream 重构
   - 编译项目，确保无编译错误
   - 运行全部 63 个现有 H1 测试，确保全部通过，行为无变化
   - 修复重构过程中引入的任何问题
   - _需求：2.4、8.1_

- [ ] 5. 扩展 `xHttpProto` vtable 并实现 H1 响应多态
   - 在 `server_private.h` 的 `xHttpProto_` 中新增三个函数指针：`send_response(struct xHttpStream_ *stream, int status, struct xHttpHeader_ *headers, const char *body, size_t body_len)`、`write_data(struct xHttpStream_ *stream, const char *data, size_t len)`、`end_stream(struct xHttpStream_ *stream)`
   - 将 `method` 签名从 `method(struct xHttpConn_ *conn)` 改为 `method(struct xHttpStream_ *stream)`
   - 在 `proto_h1.c` 中实现 `h1_send_response`：生成 HTTP/1.1 状态行 + 头部 + body 写入 `stream->conn->write_buf`（从现有 `xHttpResponseSend` 提取逻辑）
   - 在 `proto_h1.c` 中实现 `h1_write_data`：追加数据到 `stream->conn->write_buf`
   - 在 `proto_h1.c` 中实现 `h1_end_stream`：标记 stream 完成
   - 在 `proto_h1.c` 中更新 `h1_method`：从 `conn` 参数改为 `stream` 参数
   - 改造 `server.c` 中的 `xHttpResponseSend`、`xHttpResponseWrite`、`xHttpResponseEnd`：委托给 `conn->proto.send_response` / `write_data` / `end_stream`
   - 在 `xHttpProtoH1Init` 中注册新的 vtable 方法
   - _需求：4.1、4.2、4.3、4.4、4.5、4.6_

- [ ] 6. 运行测试验证 vtable 扩展
   - 编译项目，确保无编译错误
   - 运行全部现有测试，确保响应行为不变
   - _需求：8.2_

- [ ] 7. 实现协议自动检测（Prior Knowledge）
   - 在 `server_private.h` 中为 `xHttpConn_` 新增 `proto_detected`（int）字段，标记协议是否已检测
   - 修改 `conn_init_parser`：不再直接调用 `xHttpProtoH1Init`，而是将 `proto_detected` 设为 0
   - 修改 `on_conn_event` 的读取逻辑：当 `proto_detected == 0` 时，检查 `read_buf` 中的数据是否以 24 字节 HTTP/2 connection preface（`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`）开头
   - 如果匹配 H2 magic 则调用 `xHttpProtoH2Init(conn)`，否则调用 `xHttpProtoH1Init(conn)`，然后设置 `proto_detected = 1`
   - 如果数据不足 24 字节，缓冲并等待下次读取事件（但如果已有数据明确不匹配 H2 magic 前缀，则立即选择 H1）
   - 协议检测完成后，将已缓冲的全部数据传递给 `conn->proto.on_data`
   - _需求：5.1、5.2、5.3、5.4、5.5_

- [ ] 8. CMake 集成 nghttp2
   - 在 `xhttp/CMakeLists.txt` 中添加 `find_package(Nghttp2 REQUIRED)`
   - 添加 `target_compile_definitions(xhttp PUBLIC XK_HAS_NGHTTP2)`
   - 添加 `target_link_libraries(xhttp PRIVATE Nghttp2::Nghttp2)`
   - 验证编译通过（`proto_h2.c` 会被 `GLOB_RECURSE` 自动包含）
   - _需求：7.1、7.2、7.3_

- [ ] 9. 实现 `proto_h2.c` HTTP/2 协议处理器
   - 创建 `xhttp/proto_h2.h`：声明 `xHttpProtoH2Init(struct xHttpConn_ *conn)`
   - 创建 `xhttp/proto_h2.c`：定义 `xHttpProtoH2_` 内部状态结构体，持有 `nghttp2_session *session`
   - 实现 `xHttpProtoH2Init`：创建 `nghttp2_session_server_new2()`，注册回调，发送 server connection preface（SETTINGS 帧），填充 vtable
   - 实现 nghttp2 回调 `on_begin_headers_callback`：为新 stream 调用 `xHttpStreamCreate(conn, stream_id)`，通过 `nghttp2_session_set_stream_user_data` 关联
   - 实现 nghttp2 回调 `on_header_callback`：将 `:method` 写入 stream 的 method 字段，`:path` 写入 `stream->url`，普通头写入 `stream->headers_raw`
   - 实现 nghttp2 回调 `on_data_chunk_recv_callback`：将 body 数据写入 `stream->body`
   - 实现 nghttp2 回调 `on_frame_recv_callback`：当 HEADERS/DATA 帧带有 END_STREAM 标志时，标记 `stream->request_complete = 1` 并触发 dispatch
   - 实现 nghttp2 回调 `on_stream_close_callback`：调用 `xHttpStreamDestroy` 销毁 stream
   - 实现 nghttp2 `send_callback`：将帧数据写入 `conn->write_buf`
   - 实现 vtable `h2_on_data`：调用 `nghttp2_session_mem_recv()` + `nghttp2_session_send()`
   - 实现 vtable `h2_send_response`：构造 nghttp2 nv 数组，调用 `nghttp2_submit_response()`，然后 `nghttp2_session_send()`
   - 实现 vtable `h2_write_data` 和 `h2_end_stream`：通过 nghttp2 data provider 机制发送 DATA 帧
   - 实现 vtable `h2_reset`、`h2_destroy`、`h2_method`、`h2_should_keep_alive`
   - _需求：6.1、6.2、6.3、6.4、6.5、6.6、6.7、6.8、6.9、6.10_

- [ ] 10. 编写 HTTP/2 测试
   - 在 `server_test.cpp` 中新增 H2 测试 fixture，使用 nghttp2 客户端 API 或原始 socket 发送 H2 帧
   - 测试 Prior Knowledge 协议检测：发送 H2 magic + SETTINGS + HEADERS，验证收到正确响应
   - 测试单 stream H2 请求/响应：GET 和 POST 请求，验证状态码、头部、body 正确
   - 测试多 stream 并发请求：在同一连接上发送多个并发 stream，验证所有响应正确
   - 测试 H2 stream 错误处理：发送无效帧，验证服务端正确处理
   - 测试 H2 GOAWAY 优雅关闭
   - 测试 H1/H2 共存：同一端口先后建立 H1 和 H2 连接，验证两者都正常工作
   - _需求：7.4、8.3、8.4_
