# 实施计划

- [ ] 1. 创建 `cmake/FindLibunwind.cmake` 自定义查找模块
   - 使用 `find_path` 查找 `<libunwind.h>` 头文件
   - 使用 `find_library` 查找 `libunwind` 库文件
   - 使用 `find_package_handle_standard_args` 设置 `Libunwind_FOUND`、`Libunwind_INCLUDE_DIRS`、`Libunwind_LIBRARIES` 标准变量
   - 创建 `Libunwind::Libunwind` imported target（IMPORTED INTERFACE 库）
   - _需求：6.2_

- [ ] 2. 修改 CMake 构建系统，集成后端检测逻辑
   - 在顶层 `CMakeLists.txt` 中将 `cmake/` 添加到 `CMAKE_MODULE_PATH`（移到更早的位置，使其不仅限于测试构建）
   - 在 `xbase/CMakeLists.txt` 中添加后端检测逻辑：先 `find_package(Libunwind)`，若找到则 `target_compile_definitions(xbase PRIVATE XK_HAS_LIBUNWIND)` 并链接 `Libunwind::Libunwind`
   - 若未找到 libunwind，使用 `check_include_file` 检测 `<execinfo.h>`，若可用则定义 `XK_HAS_EXECINFO`
   - 在 Linux + execinfo 场景下添加 `-rdynamic` 链接选项
   - 源文件由 `GLOB_RECURSE` 自动收集，无需手动添加
   - _需求：6.1, 6.3, 6.4, 6.5, 6.6, 6.7_

- [ ] 3. 创建 `xbase/backtrace.h` 头文件
   - 添加版权声明和头文件保护宏（`XBASE_BACKTRACE_H`），风格与 `throw.h` 一致
   - 包含 `<stddef.h>`（for `size_t`）和 `<xbase/base.h>`（for `XCAPI`）
   - 声明 `XCAPI(int) xBacktrace(char *buf, size_t size)`
   - 声明 `XCAPI(int) xBacktraceSkip(int skip, char *buf, size_t size)`
   - 添加 Doxygen 风格注释说明每个函数的用途和参数
   - _需求：1.5, 2.2, 2.3_

- [ ] 4. 实现 `xbase/backtrace.c` — libunwind 后端
   - 在 `#if defined(XK_HAS_LIBUNWIND)` 条件编译块中实现
   - 使用 `unw_getcontext` + `unw_init_local` + `unw_step` 遍历调用栈
   - 使用 `unw_get_reg` 获取 IP 地址，`unw_get_proc_name` 获取符号名和偏移
   - 根据 `skip` 参数跳过指定帧数（`xBacktraceSkip` 自身 + 用户指定的额外帧数）
   - 使用 `snprintf` 安全格式化每帧为 `#N 0xADDR symbol+offset` 或 `#N 0xADDR <unknown>`
   - 处理 `buf == NULL || size == 0` 的防御性返回
   - 保证截断时 `buf` 以 `\0` 结尾
   - _需求：1.1, 1.2, 1.3, 1.4, 2.1, 2.2, 3.2, 3.3_

- [ ] 5. 实现 `xbase/backtrace.c` — execinfo 后端
   - 在 `#elif defined(XK_HAS_EXECINFO)` 条件编译块中实现
   - 使用 `backtrace()` 获取帧地址数组，`backtrace_symbols()` 获取符号字符串
   - 根据 `skip` 参数跳过指定帧数
   - 解析 `backtrace_symbols` 返回的字符串，格式化为统一的 `#N 0xADDR symbol+offset` 格式
   - 使用 `free()` 释放 `backtrace_symbols` 返回的内存
   - 同样处理 NULL/0 防御和截断保证
   - _需求：1.1, 1.2, 1.3, 1.4, 2.1, 2.2, 4.2, 4.3_

- [ ] 6. 实现 `xbase/backtrace.c` — stub 后端
   - 在 `#else` 条件编译块中实现
   - `xBacktraceSkip` 写入固定字符串 `"<backtrace not supported on this platform>\n"`
   - 返回该字符串的长度（或截断后的长度）
   - 同样处理 NULL/0 防御
   - _需求：5.1, 5.2, 5.3_

- [ ] 7. 实现 `xBacktrace` 包装函数
   - `xBacktrace(buf, size)` 内部调用 `xBacktraceSkip(0, buf, size)`，自动跳过自身 1 帧（在 `xBacktraceSkip` 实现中 skip 基数设为包含自身的帧数）
   - 确保 `xBacktrace` 调用链中的帧跳过计数正确（`xBacktrace` → `xBacktraceSkip` → 后端，需跳过 2 帧 + 用户 skip）
   - _需求：2.3_

- [ ] 8. 编写 `xbase/backtrace_test.cpp` 单元测试
   - 使用 Google Test 框架，与现有 `throw_test.cpp` 风格一致
   - 测试 `xBacktrace` 基本功能：返回值 > 0，输出包含 `#0`
   - 测试 NULL buf 和 size=0 场景：返回值为 0，无崩溃
   - 测试小缓冲区截断：size=16 时输出被截断且以 `\0` 结尾
   - 测试 `xBacktraceSkip` 不同 skip 值：帧数随 skip 增大而减少
   - 测试输出格式：每帧匹配 `#N 0x` 前缀模式
   - _需求：7.1, 7.2, 7.3, 7.4, 7.5_

- [ ] 9. 编译验证与测试运行
   - 执行 CMake 配置，确认后端检测日志输出正确（显示使用了哪个后端）
   - 编译 xbase 库和测试，确认无编译警告
   - 运行 `backtrace_test` 全部测试用例通过
   - _需求：6.1, 7.1 ~ 7.5_
