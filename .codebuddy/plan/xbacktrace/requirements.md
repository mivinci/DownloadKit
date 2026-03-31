# 需求文档

## 引言

xBacktrace 模块为 xKit 提供一个独立的、按需调用的堆栈回溯（stack unwinding）功能。该模块以 `backtrace.h` / `backtrace.c` 的形式存在于 `xbase/` 目录下，遵循项目现有的 `XCAPI` 宏风格和版权头规范。

核心设计原则：
- **独立模块** — 不侵入 `xThrow`，用户可在回调中按需调用
- **平台自适应** — 根据编译环境自动选择最佳后端：优先 `libunwind`，其次 `execinfo.h`（macOS/Linux glibc），最后提供一个 stub 实现
- **后端选择在 CMake 层完成** — 通过 `check_include_file` / `find_library` 探测可用性，以宏定义传递给源码

## 需求

### 需求 1：公共 API 定义

**用户故事：** 作为一名 C 开发者，我希望有一个简洁的函数将当前调用栈格式化到缓冲区中，以便在错误处理或日志场景中快速获取堆栈信息。

#### 验收标准

1. WHEN 用户调用 `xBacktrace(buf, size)` THEN 系统 SHALL 将当前调用栈以人类可读的多行文本写入 `buf`，并返回实际写入的字节数（不含 `\0`）。
2. WHEN `buf` 为 NULL 或 `size` 为 0 THEN 系统 SHALL 安全返回 0，不产生任何副作用。
3. WHEN 堆栈文本超过 `size - 1` 字节 THEN 系统 SHALL 截断输出并保证 `buf` 以 `\0` 结尾。
4. 每一帧的输出格式 SHALL 为 `#N 0xADDR symbol+offset` 或 `#N 0xADDR <unknown>`（当符号不可用时），每帧占一行。
5. 函数签名 SHALL 为 `XCAPI(int) xBacktrace(char *buf, size_t size)`，声明在 `xbase/backtrace.h` 中。

### 需求 2：跳帧控制

**用户故事：** 作为一名 C 开发者，我希望能控制跳过栈顶的若干帧（如 `xBacktrace` 自身、`xThrow` 等内部帧），以便输出的堆栈从真正有意义的调用点开始。

#### 验收标准

1. WHEN 用户调用 `xBacktrace` THEN 系统 SHALL 自动跳过 `xBacktrace` 函数自身所在的帧（至少跳过 1 帧）。
2. IF 需要更灵活的跳帧控制 THEN 系统 SHALL 提供 `XCAPI(int) xBacktraceSkip(int skip, char *buf, size_t size)` 函数，其中 `skip` 表示额外跳过的帧数（0 表示不额外跳过）。
3. `xBacktrace(buf, size)` SHALL 等价于 `xBacktraceSkip(0, buf, size)`（内部已自动跳过自身 1 帧）。

### 需求 3：平台后端 — libunwind

**用户故事：** 作为一名跨平台 C 开发者，我希望在有 `libunwind` 的环境下自动使用它作为堆栈回溯后端，以便获得最可靠、最详细的堆栈信息。

#### 验收标准

1. WHEN CMake 检测到系统安装了 `libunwind`（头文件 `<libunwind.h>` 和库文件可用）THEN 构建系统 SHALL 定义宏 `XK_HAS_LIBUNWIND` 并链接 `libunwind`。
2. WHEN `XK_HAS_LIBUNWIND` 被定义 THEN `backtrace.c` SHALL 使用 `unw_backtrace` / `unw_getcontext` + `unw_step` 系列 API 进行堆栈回溯。
3. WHEN libunwind 能解析符号名 THEN 系统 SHALL 输出 `symbol+offset` 格式；OTHERWISE SHALL 输出 `<unknown>`。

### 需求 4：平台后端 — execinfo（macOS / Linux glibc）

**用户故事：** 作为一名 macOS 或 Linux 开发者，我希望在没有 `libunwind` 但有 `<execinfo.h>` 的环境下仍能获取堆栈回溯，以便在常见平台上开箱即用。

#### 验收标准

1. WHEN CMake 未检测到 `libunwind` 但检测到 `<execinfo.h>` 头文件可用 THEN 构建系统 SHALL 定义宏 `XK_HAS_EXECINFO`。
2. WHEN `XK_HAS_EXECINFO` 被定义 THEN `backtrace.c` SHALL 使用 `backtrace()` + `backtrace_symbols()` 进行堆栈回溯。
3. 系统 SHALL 在文档或注释中提示：使用 execinfo 后端时，需要 `-rdynamic` 链接选项才能获得完整符号名。

### 需求 5：Stub 后端（不支持的平台）

**用户故事：** 作为一名开发者，我希望在不支持堆栈回溯的平台上代码仍能编译通过，以便保持项目的可移植性。

#### 验收标准

1. WHEN 既没有 `libunwind` 也没有 `<execinfo.h>` THEN 构建系统 SHALL 不定义上述任何宏。
2. WHEN 没有任何后端宏被定义 THEN `xBacktrace` / `xBacktraceSkip` SHALL 写入固定字符串 `"<backtrace not supported on this platform>\n"` 并返回该字符串的长度。
3. stub 实现 SHALL 不引入任何编译警告。

### 需求 6：CMake 构建集成

**用户故事：** 作为一名构建维护者，我希望后端检测和库链接在 CMake 层自动完成，以便开发者无需手动配置。

#### 验收标准

1. WHEN 构建 xbase 库 THEN CMake SHALL 按优先级顺序检测：libunwind → execinfo → stub。
2. 系统 SHALL 提供 `cmake/FindLibunwind.cmake` 自定义查找模块，用于检测 libunwind 的头文件和库文件。
   - 该模块 SHALL 使用 `find_path` 查找 `<libunwind.h>` 头文件。
   - 该模块 SHALL 使用 `find_library` 查找 `libunwind` 库文件。
   - 该模块 SHALL 使用 `find_package_handle_standard_args` 设置 `Libunwind_FOUND`、`Libunwind_INCLUDE_DIRS`、`Libunwind_LIBRARIES` 等标准变量。
   - 该模块 SHALL 创建 `Libunwind::Libunwind` imported target 以便现代 CMake 风格使用。
3. 顶层 `CMakeLists.txt` SHALL 将 `cmake/` 目录添加到 `CMAKE_MODULE_PATH`，以便 `find_package(Libunwind)` 能找到自定义模块。
4. WHEN 检测到 libunwind THEN CMake SHALL 自动将 `Libunwind::Libunwind` 添加到 `xbase` 的链接依赖中，并定义 `XK_HAS_LIBUNWIND` 编译宏。
5. WHEN 未检测到 libunwind THEN CMake SHALL 使用 `check_include_file` 检测 `<execinfo.h>`，若可用则定义 `XK_HAS_EXECINFO` 编译宏。
6. WHEN 使用 execinfo 后端 THEN CMake SHALL 在 Linux 平台上自动添加 `-rdynamic` 链接选项。
7. 新增的 `backtrace.h` / `backtrace.c` SHALL 被 `GLOB_RECURSE` 自动收集，无需手动修改源文件列表。

### 需求 7：单元测试

**用户故事：** 作为一名开发者，我希望有充分的测试覆盖堆栈回溯功能，以便确保各后端实现的正确性。

#### 验收标准

1. WHEN 运行测试 THEN 系统 SHALL 验证 `xBacktrace` 返回值 > 0 且 `buf` 中包含至少一个帧（含 `#0`）。
2. WHEN `buf` 为 NULL 或 `size` 为 0 THEN 测试 SHALL 验证返回值为 0 且无崩溃。
3. WHEN `size` 很小（如 16 字节）THEN 测试 SHALL 验证输出被正确截断且以 `\0` 结尾。
4. WHEN 调用 `xBacktraceSkip` 并传入不同 `skip` 值 THEN 测试 SHALL 验证输出帧数随 `skip` 增大而减少。
5. 测试文件 SHALL 命名为 `backtrace_test.cpp`，使用 Google Test 框架，与现有测试风格一致。
