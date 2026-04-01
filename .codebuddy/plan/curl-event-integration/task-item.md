# 实施计划

- [ ] 1. 编写 `cmake/FindLibcurl.cmake` 并集成到构建系统
   - 参照 `cmake/FindLibunwind.cmake` 的风格，编写 `cmake/FindLibcurl.cmake`，查找 `curl/curl.h` 头文件和 `libcurl` 库
   - 成功时设置 `LIBCURL_FOUND`、`LIBCURL_INCLUDE_DIRS`、`LIBCURL_LIBRARIES`，并创建 `Libcurl::Libcurl` imported target
   - 未找到时将 `LIBCURL_FOUND` 设为 FALSE，不产生错误
   - 在根 `CMakeLists.txt` 中添加 `find_package(Libcurl QUIET)`，找到时 `add_subdirectory(xhttp)`
   - _需求：6.1, 6.2, 6.3, 6.4, 6.5_

- [ ] 2. 创建 `xhttp` 模块目录和 `xhttp/CMakeLists.txt`
   - 创建 `xhttp/` 目录
   - 编写 `xhttp/CMakeLists.txt`，链接 `xbase` 和 `Libcurl::Libcurl`，定义 `XK_HAS_CURL` 编译宏
   - 配置测试目标，链接 GTest
   - _需求：6.4, 6.6_

- [ ] 3. 定义公共 API 头文件 `xhttp/http.h`
   - 定义 `xHttpClient` 句柄（`XDEF_HANDLE`）
   - 定义 `xHttpResponse` 结构体，包含 HTTP 状态码、响应头、响应体（指针+长度）、curl 错误码和错误描述
   - 定义 `xHttpResponseFunc` 回调类型
   - 声明 `xHttpClientCreate(xEventLoop loop)` 和 `xHttpClientDestroy(xHttpClient client)`
   - 声明 `xHttpClientGet(client, url, on_response, arg)` 和 `xHttpClientPost(client, url, body, body_len, on_response, arg)`
   - 声明 `xHttpRequest` 配置结构体（支持自定义 headers、method、timeout）及 `xHttpClientDo(client, request, on_response, arg)` 通用请求接口
   - _需求：1.1, 1.2, 2.1, 2.2, 2.3, 5.3, 5.4_

- [ ] 4. 实现内部数据结构 `xhttp/http_base.h`
   - 定义 `struct xHttpClient_`，包含 `CURLM *multi`、`xEventLoop loop`、`xEventTimer timer`（curl 超时定时器）
   - 定义 `struct xHttpRequest_`（per-request 上下文），包含 `CURL *easy`、`xHttpClient client`（回指）、`xHttpResponseFunc on_response`、`void *arg`、响应数据动态缓冲区（body buf + header buf）
   - 定义 `struct xHttpSocketCtx_`（per-socket 上下文），包含 `xEventSource src`、`int fd`、`xHttpClient client`（回指）
   - _需求：3.1, 3.4, 4.1_

- [ ] 5. 实现 curl socket 回调与 xEventLoop fd 管理集成
   - 实现 `socket_callback(CURL *easy, curl_socket_t fd, int what, void *userp, void *socketp)`：
     - `CURL_POLL_IN` / `CURL_POLL_OUT` / `CURL_POLL_INOUT`：映射为 `xEvent_Read` / `xEvent_Write` / 两者，调用 `xEventAdd` 或 `xEventMod`
     - `CURL_POLL_REMOVE`：调用 `xEventDel` 移除监听，`curl_multi_assign` 清除关联，释放 socket 上下文
   - 实现 `fd_ready_callback(int fd, xEventMask mask, void *arg)`：将 xEventMask 转换为 `CURL_CSELECT_IN` / `CURL_CSELECT_OUT`，调用 `curl_multi_socket_action`，然后调用完成检查函数
   - _需求：3.1, 3.2, 3.3, 3.4, 3.5, 3.6_

- [ ] 6. 实现 curl timer 回调与 xEventLoop 定时器集成
   - 实现 `timer_callback(CURLM *multi, long timeout_ms, void *userp)`：
     - `timeout_ms >= 1`：取消旧定时器，调用 `xEventLoopTimerAfter` 设置新定时器
     - `timeout_ms == 0`：立即调用 `curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running)` 并检查完成
     - `timeout_ms == -1`：取消当前定时器
   - 实现 `on_timeout(void *arg)`：调用 `curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running)` 并检查完成
   - _需求：4.1, 4.2, 4.3, 4.4_

- [ ] 7. 实现传输完成检查与回调分发
   - 实现 `check_multi_info(xHttpClient client)` 辅助函数：
     - 循环调用 `curl_multi_info_read(multi, &msgs_in_queue)` 获取已完成的传输
     - 对每个 `CURLMSG_DONE` 消息，通过 `curl_easy_getinfo(CURLINFO_PRIVATE)` 获取 `xHttpRequest_` 上下文
     - 通过 `curl_easy_getinfo(CURLINFO_RESPONSE_CODE)` 获取 HTTP 状态码
     - 组装 `xHttpResponse` 结构体，调用用户的 `on_response` 回调
     - 调用 `curl_multi_remove_handle` 和 `curl_easy_cleanup` 清理 easy handle
     - 释放请求上下文的动态缓冲区
   - _需求：3.6, 5.3, 5.4, 5.5_

- [ ] 8. 实现 HTTP 客户端生命周期管理（Create / Destroy）
   - 实现 `xHttpClientCreate(xEventLoop loop)`：
     - 调用 `curl_multi_init()`，失败返回 NULL
     - 通过 `curl_multi_setopt` 注册 socket 回调和 timer 回调
     - 返回 `xHttpClient` 句柄
   - 实现 `xHttpClientDestroy(xHttpClient client)`：
     - 遍历所有进行中的 easy handle，对每个调用完成回调传入错误状态（`CURLE_ABORTED_BY_CALLBACK`）
     - 移除所有 xEventSource 和 xEventTimer
     - 调用 `curl_multi_cleanup()`，释放所有内部资源
   - _需求：1.1, 1.2, 1.3, 1.4_

- [ ] 9. 实现异步 HTTP 请求提交（Get / Post / Do）
   - 实现响应数据收集回调 `write_callback`（`CURLOPT_WRITEFUNCTION`）和 `header_callback`（`CURLOPT_HEADERFUNCTION`），将数据追加到请求上下文的动态缓冲区
   - 实现 `xHttpClientGet`：创建 easy handle，配置 URL、write/header 回调、`CURLOPT_PRIVATE`，调用 `curl_multi_add_handle`
   - 实现 `xHttpClientPost`：同上，额外配置 `CURLOPT_POST`、`CURLOPT_POSTFIELDS`、`CURLOPT_POSTFIELDSIZE`
   - 实现 `xHttpClientDo`：支持自定义 headers、method、timeout 等配置
   - `curl_multi_add_handle` 失败时返回错误码，不调用回调
   - _需求：2.1, 2.2, 2.3, 2.4, 2.5, 5.1, 5.2_

- [ ] 10. 编写单元测试 `xhttp/http_test.cpp`
   - 客户端创建和销毁测试
   - 单个 GET 请求完整生命周期测试（可对 httpbin.org 或本地 mock 发起请求）
   - 单个 POST 请求完整生命周期测试
   - 多个并发请求测试
   - 请求失败场景测试（无效 URL、连接超时）
   - 客户端销毁时取消进行中请求的测试
   - libcurl 不可用时测试自动跳过
   - _需求：7.1, 7.2_
