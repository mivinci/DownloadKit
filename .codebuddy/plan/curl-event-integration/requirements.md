# 需求文档：libcurl 与 xEventLoop 集成

## 引言

本功能旨在将 libcurl 的 multi socket API (`curl_multi_socket_action`) 与 xKit 的 `xEventLoop` 进行深度集成，提供一个事件驱动的异步 HTTP 客户端模块。

### 背景

xKit 已有一个跨平台的 edge-triggered 事件循环（支持 kqueue/epoll/poll 后端），具备 fd 监听、定时器、线程池卸载等完整能力。libcurl 的 multi socket API 允许外部事件循环接管 I/O 调度，通过 `CURLMOPT_SOCKETFUNCTION` 和 `CURLMOPT_TIMERFUNCTION` 两个回调将 fd 监听和超时管理委托给外部。

### 设计目标

- 将 libcurl 的 socket/timer 事件完全托管给 xEventLoop，实现单线程非阻塞 HTTP 并发
- 封装为独立模块（`xhttp`），对外提供简洁的异步 HTTP 客户端 API
- 遵循 xKit 现有的命名规范（`xHttpXxx`）、句柄模式（`XDEF_HANDLE`）和错误处理风格（`xErrno`）
- libcurl 作为可选外部依赖，通过 CMake 的 `find_package(CURL)` 发现

### 约束

- 所有回调（完成回调、fd 就绪回调、timer 回调）均在 xEventLoop 所在线程上执行，无需额外同步
- xEventLoop 是 edge-triggered 的，curl 内部会将 socket 读/写到 `EAGAIN`，天然兼容 ET 模式
- 模块不引入额外线程，所有 I/O 由事件循环驱动

---

## 需求

### 需求 1：HTTP 客户端生命周期管理

**用户故事：** 作为一名 xKit 库用户，我希望能够创建和销毁一个绑定到 xEventLoop 的 HTTP 客户端实例，以便在事件循环中发起异步 HTTP 请求。

#### 验收标准

1. WHEN 用户调用 `xHttpClientCreate(loop)` THEN 系统 SHALL 返回一个有效的 `xHttpClient` 句柄，内部初始化 `curl_multi_init()` 并注册 socket/timer 回调
2. IF `curl_multi_init()` 失败 THEN 系统 SHALL 返回 NULL
3. WHEN 用户调用 `xHttpClientDestroy(client)` THEN 系统 SHALL 取消所有进行中的请求、移除所有已注册的 xEventSource 和 xEventTimer、调用 `curl_multi_cleanup()`，并释放所有内部资源
4. IF 销毁时仍有进行中的请求 THEN 系统 SHALL 对每个未完成请求调用其完成回调，传入错误状态（如 `CURLE_ABORTED_BY_CALLBACK`），然后再释放资源

### 需求 2：异步 HTTP 请求提交

**用户故事：** 作为一名 xKit 库用户，我希望能够提交异步 HTTP 请求（GET/POST 等），并在请求完成时通过回调获取结果，以便实现非阻塞的网络通信。

#### 验收标准

1. WHEN 用户调用 `xHttpClientGet(client, url, on_response, arg)` THEN 系统 SHALL 创建一个 `curl_easy_handle`，配置 URL，通过 `curl_multi_add_handle()` 加入 multi 会话，并在请求完成时在事件循环线程上调用 `on_response`
2. WHEN 用户调用 `xHttpClientPost(client, url, body, body_len, on_response, arg)` THEN 系统 SHALL 创建一个带有请求体的 `curl_easy_handle`，配置 POST 方法和数据，并在完成时回调
3. WHEN 用户需要设置自定义 HTTP 头 THEN 系统 SHALL 提供 `xHttpRequest` 构建器或配置结构体，允许设置 headers、method、timeout 等参数
4. IF `curl_multi_add_handle()` 失败 THEN 系统 SHALL 返回错误码，不调用回调
5. WHEN 请求完成（成功或失败）THEN 回调 SHALL 接收到包含 HTTP 状态码、响应头、响应体的 `xHttpResponse` 结构体

### 需求 3：curl socket 回调与 xEventLoop fd 管理集成

**用户故事：** 作为一名 xKit 库开发者，我希望 curl 的 socket 事件能自动映射到 xEventLoop 的 fd 监听机制，以便事件循环能驱动 curl 的 I/O。

#### 验收标准

1. WHEN curl 通过 `CURLMOPT_SOCKETFUNCTION` 回调通知 `CURL_POLL_IN` THEN 系统 SHALL 调用 `xEventAdd(loop, fd, xEvent_Read, ...)` 或 `xEventMod(loop, src, xEvent_Read)` 注册/更新读事件
2. WHEN curl 通知 `CURL_POLL_OUT` THEN 系统 SHALL 注册/更新写事件（`xEvent_Write`）
3. WHEN curl 通知 `CURL_POLL_INOUT` THEN 系统 SHALL 注册/更新读写事件（`xEvent_Read | xEvent_Write`）
4. WHEN curl 通知 `CURL_POLL_REMOVE` THEN 系统 SHALL 调用 `xEventDel(loop, src)` 移除该 fd 的监听，并通过 `curl_multi_assign()` 清除关联数据
5. WHEN xEventLoop 通知某个 fd 就绪 THEN 系统 SHALL 将事件掩码转换为 curl 的 `CURL_CSELECT_IN` / `CURL_CSELECT_OUT` 并调用 `curl_multi_socket_action(multi, fd, bitmask, &running)`
6. WHEN `curl_multi_socket_action` 返回后 THEN 系统 SHALL 调用 `curl_multi_info_read()` 检查已完成的传输并分发完成回调

### 需求 4：curl timer 回调与 xEventLoop 定时器集成

**用户故事：** 作为一名 xKit 库开发者，我希望 curl 的超时管理能自动映射到 xEventLoop 的内置定时器，以便 curl 的内部超时机制正常工作。

#### 验收标准

1. WHEN curl 通过 `CURLMOPT_TIMERFUNCTION` 回调请求设置超时（`timeout_ms >= 0`）THEN 系统 SHALL 取消之前的定时器（如果存在），并调用 `xEventLoopTimerAfter(loop, on_timeout, ctx, timeout_ms)` 设置新定时器
2. WHEN curl 请求 `timeout_ms == 0` THEN 系统 SHALL 立即调用 `curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running)` 而不设置定时器
3. WHEN curl 请求 `timeout_ms == -1` THEN 系统 SHALL 取消当前定时器（无需超时）
4. WHEN 定时器到期 THEN 系统 SHALL 调用 `curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running)` 并检查已完成的传输

### 需求 5：响应数据收集

**用户故事：** 作为一名 xKit 库用户，我希望系统能自动收集 HTTP 响应的状态码、头部和正文数据，以便我在回调中直接使用完整的响应信息。

#### 验收标准

1. WHEN curl 接收到响应数据 THEN 系统 SHALL 通过 `CURLOPT_WRITEFUNCTION` 回调将数据追加到内部动态缓冲区
2. WHEN curl 接收到响应头 THEN 系统 SHALL 通过 `CURLOPT_HEADERFUNCTION` 回调收集头部信息
3. WHEN 传输完成 THEN 系统 SHALL 通过 `curl_easy_getinfo()` 获取 HTTP 状态码，并将状态码、响应头、响应体封装到 `xHttpResponse` 结构体中传递给完成回调
4. WHEN 传输失败 THEN `xHttpResponse` SHALL 包含 curl 错误码和错误描述字符串，HTTP 状态码为 0
5. WHEN 完成回调返回后 THEN 系统 SHALL 自动释放 `xHttpResponse` 内部的缓冲区和 `curl_easy_handle`，用户无需手动释放

### 需求 6：构建系统集成

**用户故事：** 作为一名 xKit 库的构建维护者，我希望 libcurl 作为可选依赖被集成到 CMake 构建系统中，以便在 curl 不可用时不影响其他模块的编译。

#### 验收标准

1. WHEN 项目构建 THEN 系统 SHALL 提供 `cmake/FindLibcurl.cmake` 自定义查找模块（与现有的 `FindGTest.cmake`、`FindLibunwind.cmake` 风格一致），负责定位 libcurl 的头文件和库文件
2. WHEN `FindLibcurl.cmake` 成功找到 libcurl THEN 模块 SHALL 设置以下 CMake 变量：`LIBCURL_FOUND`、`LIBCURL_INCLUDE_DIRS`、`LIBCURL_LIBRARIES`，并创建 `Libcurl::Libcurl` imported target
3. WHEN `FindLibcurl.cmake` 未找到 libcurl THEN 模块 SHALL 将 `LIBCURL_FOUND` 设为 FALSE，不产生错误
4. WHEN CMake 配置阶段通过 `FindLibcurl.cmake` 找到 libcurl THEN 系统 SHALL 编译 xhttp 模块并链接 `Libcurl::Libcurl` target
5. IF libcurl 未找到 THEN 系统 SHALL 跳过 xhttp 模块的编译，不产生错误，其他模块正常构建
6. WHEN xhttp 模块被编译 THEN 系统 SHALL 定义 `XK_HAS_CURL` 宏，供条件编译使用

### 需求 7：单元测试

**用户故事：** 作为一名 xKit 库开发者，我希望有完善的单元测试覆盖 HTTP 客户端的核心功能，以便确保集成的正确性和稳定性。

#### 验收标准

1. WHEN 运行测试套件 THEN 系统 SHALL 包含以下测试用例：
   - 客户端创建和销毁
   - 单个 GET 请求的完整生命周期
   - 单个 POST 请求的完整生命周期
   - 多个并发请求
   - 请求失败场景（无效 URL、连接超时等）
   - 客户端销毁时取消进行中的请求
2. IF libcurl 不可用 THEN 相关测试 SHALL 被自动跳过
