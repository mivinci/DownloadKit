# 实施计划：xhttp 协议抽象层（Step 1）

- [ ] 1. 在 `server_private.h` 中定义 `xHttpProto` vtable 结构体
   - 新增 `xHttpProto` 结构体，包含 5 个函数指针（`on_data`、`reset`、`destroy`、`method`、`should_keep_alive`）和 1 个 `void *state` 字段
   - 在 `xHttpConn_` 中用 `xHttpProto proto` 字段替换 `llhttp_t parser` 和 `llhttp_settings_t parser_settings`
   - 移除 `server_private.h` 顶部的 `#include <llhttp.h>`
   - _需求：1.1, 1.2, 2.1, 2.2_

- [ ] 2. 创建 `xhttp/proto_h1.h` 内部头文件
   - 声明 `xHttpProtoH1Init(struct xHttpConn_ *conn)` 初始化函数
   - 该头文件仅供 `server.c` 和 `proto_h1.c` 内部使用，不对外暴露
   - _需求：3.1, 5.3_

- [ ] 3. 创建 `xhttp/proto_h1.c` 并实现 `xHttpProtoH1` 结构体与 llhttp 回调
   - 定义 `xHttpProtoH1` 结构体（内含 `llhttp_t parser` 和 `llhttp_settings_t settings`）
   - 从 `server.c` 搬迁 6 个 llhttp 回调函数：`on_url`、`on_header_field`、`on_header_value`、`on_headers_complete`、`on_body`、`on_message_complete`
   - 回调函数内部通过 `parser->data` 获取 `xHttpConn_` 指针，操作 conn 上的请求解析状态字段
   - `#include <llhttp.h>` 和 `#include "server_private.h"` 放在此文件中
   - _需求：3.1, 3.2_

- [ ] 4. 在 `proto_h1.c` 中实现 5 个 vtable 方法和初始化函数
   - `h1_on_data`：调用 `llhttp_execute`，封装 `pending_error` / `request_complete` 检查，返回三值结果（0/1/-1）
   - `h1_reset`：调用 `llhttp_reset` 重置 parser 状态
   - `h1_destroy`：`free` 释放 `xHttpProtoH1` 堆内存，置 `conn->proto.state = NULL`
   - `h1_method`：调用 `llhttp_method_name` 返回方法字符串
   - `h1_should_keep_alive`：调用 `llhttp_should_keep_alive` 返回结果
   - `xHttpProtoH1Init`：`calloc` 分配 `xHttpProtoH1`，初始化 llhttp parser 和 settings，将回调绑定到 settings，设置 `parser->data = conn`，填充 `conn->proto` 的所有函数指针和 `state`
   - _需求：1.3, 3.2, 3.3, 3.4, 3.5_

- [ ] 5. 改造 `server.c`：替换所有 llhttp 直接调用为 vtable 调用
   - `conn_init_parser`：删除原有 llhttp 初始化代码，改为调用 `xHttpProtoH1Init(conn)`
   - `on_conn_event` Read 分支：将 `llhttp_execute` 调用替换为 `conn->proto.on_data(conn, buf, len)`，根据三值返回值决定 dispatch / error / continue
   - `conn_dispatch_request`：将 `llhttp_method_name((llhttp_method_t)conn->parser.method)` 替换为 `conn->proto.method(conn)`
   - `conn_reset_request_state`：将 `llhttp_reset(&conn->parser)` 替换为 `conn->proto.reset(conn)`
   - `xHttpConnClose`：新增 `conn->proto.destroy(conn)` 调用释放协议状态
   - 删除 `server.c` 中的 6 个 llhttp 回调函数定义和对应的 forward declaration
   - 移除 `server.c` 中不再需要的 `#include <llhttp.h>`（如果 `server_private.h` 已不包含）
   - _需求：4.1, 4.2, 4.3, 4.4, 4.5, 4.7_

- [ ] 6. 处理 `on_headers_complete` 中的 `keep_alive` 赋值
   - 在搬迁到 `proto_h1.c` 的 `on_headers_complete` 回调中，保留 `conn->keep_alive = llhttp_should_keep_alive(parser)` 的直接赋值
   - `conn->proto.should_keep_alive` vtable 方法作为备用接口供外部查询，本阶段 `server.c` 中无需额外调用
   - _需求：4.6_

- [ ] 7. 更新 `xhttp/CMakeLists.txt` 构建配置
   - 确认 `file(GLOB_RECURSE ...)` 已自动包含新增的 `proto_h1.c`，无需手动添加
   - 如果使用显式文件列表，则将 `proto_h1.c` 添加到源文件列表
   - _需求：5.1_

- [ ] 8. 编译验证与测试
   - 本地执行 CMake 构建，确保零编译错误、零警告（`-Werror`）
   - 运行所有现有测试（`xhttp_test`），确保零修改全部通过
   - 验证 `server.h` 公开头文件无任何改动
   - _需求：5.2, 6.1, 6.2, 6.3, 6.4_
