# 需求文档：xhttp 协议抽象层（Step 1）

## 引言

本需求描述 xhttp 模块 HTTP/2 路线图中的 **Step 1**：将 `xHttpConn_` 中与 llhttp 强耦合的协议解析逻辑抽取到一个 `xHttpProto` vtable 接口后面，使连接管理层与具体协议实现解耦。

### 背景

当前 `xHttpConn_` 直接嵌入了 `llhttp_t` 和 `llhttp_settings_t` 字段，6 个 llhttp 回调函数直接操作 `xHttpConn_` 的请求解析状态，`conn_dispatch_request` 通过 `conn->parser.method` 读取 HTTP 方法。这种紧耦合使得未来引入 HTTP/2（nghttp2）时需要大量侵入式修改。

### 设计决策（已确认）

1. **`xHttpProtoH1` 采用堆分配**（`calloc`），不使用 union 内嵌
2. **llhttp 回调函数移到独立的 `proto_h1.c` 文件**，建立清晰的模块边界
3. **`on_data` 使用三值返回语义**：`0` = 继续等待，`1` = 请求完整可 dispatch，`-1` = 解析错误
4. **Response 写入路径本阶段不动**，留到 Step 3 引入 HTTP/2 时再抽象

## 需求

### 需求 1：定义 xHttpProto vtable 接口

**用户故事：** 作为一名 xhttp 开发者，我希望有一个协议处理器的抽象接口（vtable），以便将来可以透明地切换 HTTP/1.1 和 HTTP/2 的解析实现。

#### 验收标准

1. WHEN 定义 `xHttpProto` 结构体 THEN 该结构体 SHALL 包含以下函数指针：
   - `int (*on_data)(struct xHttpConn_ *conn, const char *buf, size_t len)` — 喂数据给协议解析器
   - `void (*reset)(struct xHttpConn_ *conn)` — 重置解析器状态（keep-alive 复用）
   - `void (*destroy)(struct xHttpConn_ *conn)` — 销毁协议相关状态
   - `const char *(*method)(struct xHttpConn_ *conn)` — 获取当前请求的 HTTP 方法字符串
   - `int (*should_keep_alive)(struct xHttpConn_ *conn)` — 判断连接是否应保持活跃
2. WHEN 定义 `xHttpProto` 结构体 THEN 该结构体 SHALL 包含一个 `void *state` 字段，用于存储协议实现的不透明状态指针
3. WHEN `on_data` 被调用 THEN 返回值 SHALL 遵循三值语义：`0` 表示继续等待更多数据，`1` 表示请求完整可以 dispatch，`-1` 表示解析错误

### 需求 2：改造 xHttpConn\_ 结构体

**用户故事：** 作为一名 xhttp 开发者，我希望 `xHttpConn_` 不再直接依赖 llhttp 类型，以便连接管理层对具体协议实现保持零依赖。

#### 验收标准

1. WHEN 改造 `xHttpConn_` THEN 该结构体 SHALL 用 `xHttpProto proto` 字段替换原有的 `llhttp_t parser` 和 `llhttp_settings_t parser_settings` 字段
2. WHEN 改造完成 THEN `server_private.h` SHALL 不再需要 `#include <llhttp.h>`（llhttp 的引用移到 `proto_h1.c`）
3. WHEN 改造完成 THEN 请求解析状态字段（`url`, `header_field`, `headers_raw`, `body`, `header_bytes`）SHALL 保留在 `xHttpConn_` 中不做移动
4. WHEN 改造完成 THEN `request_complete` 和 `pending_error` / `pending_error_reason` 字段 SHALL 保留在 `xHttpConn_` 中，供 `on_data` 内部设置、外部读取

### 需求 3：实现独立的 HTTP/1.1 协议处理器（proto\_h1.c）

**用户故事：** 作为一名 xhttp 开发者，我希望 HTTP/1.1 的 llhttp 解析逻辑封装在独立文件中，以便与未来的 HTTP/2 处理器保持清晰的模块边界。

#### 验收标准

1. WHEN 创建 `proto_h1.c` THEN 该文件 SHALL 包含：
   - `xHttpProtoH1` 结构体定义（内含 `llhttp_t parser` 和 `llhttp_settings_t settings`）
   - 6 个 llhttp 回调函数（`on_url`, `on_header_field`, `on_header_value`, `on_headers_complete`, `on_body`, `on_message_complete`）
   - 5 个 vtable 方法实现（`h1_on_data`, `h1_reset`, `h1_destroy`, `h1_method`, `h1_should_keep_alive`）
   - 一个公开的初始化函数，用于创建并初始化 `xHttpProtoH1` 实例
2. WHEN `xHttpProtoH1` 被创建 THEN SHALL 通过 `calloc` 堆分配，并将指针存入 `conn->proto.state`
3. WHEN `h1_on_data` 被调用 THEN SHALL 内部调用 `llhttp_execute`，并将 `pending_error` / `request_complete` 的检查逻辑封装在内部，对外只返回三值结果
4. WHEN `h1_destroy` 被调用 THEN SHALL 释放 `xHttpProtoH1` 的堆内存，并将 `conn->proto.state` 置为 `NULL`
5. WHEN `h1_reset` 被调用 THEN SHALL 调用 `llhttp_reset` 重置内部 parser 状态

### 需求 4：改造 server.c 中的调用点

**用户故事：** 作为一名 xhttp 开发者，我希望 `server.c` 通过 vtable 接口调用协议操作，以便 server 核心逻辑不再直接依赖 llhttp API。

#### 验收标准

1. WHEN 新连接被接受（`on_listen_event`）THEN `conn_init_parser` SHALL 调用 proto_h1 的初始化函数来设置 `conn->proto`
2. WHEN 收到数据（`on_conn_event` 的 Read 分支）THEN SHALL 调用 `conn->proto.on_data(conn, buf, len)` 替代直接调用 `llhttp_execute`，并根据三值返回值决定后续行为
3. WHEN 请求被 dispatch（`conn_dispatch_request`）THEN SHALL 调用 `conn->proto.method(conn)` 替代 `llhttp_method_name((llhttp_method_t)conn->parser.method)`
4. WHEN 请求状态被重置（`conn_reset_request_state`）THEN SHALL 调用 `conn->proto.reset(conn)` 替代 `llhttp_reset(&conn->parser)`
5. WHEN 连接被关闭（`xHttpConnClose`）THEN SHALL 调用 `conn->proto.destroy(conn)` 释放协议状态
6. WHEN `on_headers_complete` 回调中判断 keep-alive THEN SHALL 调用 `conn->proto.should_keep_alive(conn)` 替代 `llhttp_should_keep_alive(parser)`
7. WHEN 所有调用点改造完成 THEN `server.c` SHALL 不再包含 `#include <llhttp.h>`（该头文件仅在 `proto_h1.c` 中引用）

### 需求 5：更新构建系统

**用户故事：** 作为一名 xhttp 开发者，我希望新增的 `proto_h1.c` 文件被正确纳入构建，以便项目能正常编译。

#### 验收标准

1. WHEN `proto_h1.c` 被创建 THEN `xhttp/CMakeLists.txt` SHALL 将其添加到源文件列表中
2. WHEN 构建完成 THEN 所有现有测试 SHALL 零修改通过
3. IF `proto_h1.c` 需要引用 `server_private.h` 中的内部类型 THEN SHALL 通过 `#include "server_private.h"` 访问，不引入新的公开头文件

### 需求 6：保持外部 API 和行为不变

**用户故事：** 作为一名 xhttp 的使用者，我希望这次内部重构不影响任何公开 API 和运行时行为，以便我的代码无需任何修改。

#### 验收标准

1. WHEN 重构完成 THEN 公开头文件 `xhttp/server.h` SHALL 无任何改动
2. WHEN 重构完成 THEN 所有现有功能（路由、参数路由、SSE、流式响应、keep-alive、错误处理、超时）SHALL 行为完全一致
3. WHEN 重构完成 THEN 所有现有单元测试和集成测试 SHALL 不做修改即可通过
4. IF 重构引入了编译警告 THEN SHALL 在提交前修复所有警告（项目 CI 对 C 代码开启了 `-Werror`）
