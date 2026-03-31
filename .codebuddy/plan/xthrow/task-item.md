# 实施计划

- [ ] 1. 编写 `xbase/throw.h` 头文件，声明类型与公开 API
   - 添加版权声明、头文件保护 `#ifndef XBASE_THROW_H`
   - 包含 `<xbase/base.h>`
   - 定义 `XTHROW_BUF_SIZE` 编译期宏（默认 512），支持用户 `#define` 覆盖
   - 使用 `typedef` 定义回调类型 `xThrowCallback`：`void (*)(const char *msg, void *userdata)`
   - 声明 `XCAPI(void) xThrowSetCallback(xThrowCallback cb, void *userdata)` — 注册当前线程的回调及上下文
   - 声明 `XCAPI(void) xThrow(const char *fmt, ...)` — 格式化消息并触发回调
   - _需求：1.1, 1.2, 2.1, 3.1, 3.2, 3.3, 4.3, 5.1, 5.2, 5.3, 5.4_

- [ ] 2. 编写 `xbase/throw.c` 实现文件
   - [ ] 2.1 实现线程局部存储
      - 定义 `__thread` 级静态变量：`xThrowCallback` 和 `void *userdata`
      - 定义 `__thread static char buf[XTHROW_BUF_SIZE]` 格式化缓冲区
      - _需求：1.1, 1.3, 4.1, 4.3_
   - [ ] 2.2 实现 `xThrowSetCallback`
      - 将 `cb` 和 `userdata` 存入当前线程的 `__thread` 变量
      - 传入 `NULL` 时清除回调和 userdata
      - _需求：1.1, 1.2, 1.4, 3.3_
   - [ ] 2.3 实现 `xThrow`
      - 若 `fmt` 为 NULL，使用 `"(null)"` 作为默认消息
      - 使用 `va_start` / `vsnprintf` / `va_end` 格式化消息到线程局部缓冲区
      - 若当前线程已设置回调，调用 `cb(buf, userdata)`
      - 若未设置回调，fallback 到 `fprintf(stderr, ...)`
      - _需求：2.1, 2.2, 2.3, 4.1, 4.2_

- [ ] 3. 将 `throw.c` 加入构建系统
   - 在 `CMakeLists.txt` 中将 `xbase/throw.c` 添加到 xbase 库的源文件列表
   - 确保编译通过
   - _需求：5.1_

- [ ] 4. 编写 `xbase/throw_test.cpp` 单元测试
   - [ ] 4.1 测试基本回调注册与触发
      - 设置回调，调用 `xThrow`，验证回调收到正确的格式化消息
      - _需求：1.1, 2.1_
   - [ ] 4.2 测试 userdata 传递
      - 注册带 userdata 的回调，验证回调触发时 userdata 正确传回
      - _需求：3.1, 3.2, 3.3_
   - [ ] 4.3 测试清除回调（传 NULL）
      - 设置回调后调用 `xThrowSetCallback(NULL, NULL)`，再调用 `xThrow`，验证不触发旧回调（fallback 到 stderr）
      - _需求：1.2_
   - [ ] 4.4 测试多次覆盖回调
      - 连续设置不同回调，验证以最后一次为准
      - _需求：1.4_
   - [ ] 4.5 测试 `fmt` 为 NULL 的防御
      - 调用 `xThrow(NULL)`，验证不崩溃且回调收到 `"(null)"` 消息
      - _需求：2.3_
   - [ ] 4.6 测试消息截断（超长格式化字符串）
      - 构造超过 `XTHROW_BUF_SIZE` 的消息，验证不溢出且消息被截断
      - _需求：4.2_
   - [ ] 4.7 测试线程隔离
      - 在两个线程中分别设置不同回调，各自调用 `xThrow`，验证互不干扰
      - _需求：1.3, 6.1, 6.2_

- [ ] 5. 将测试加入构建系统并验证全部通过
   - 在 `CMakeLists.txt` 中将 `xbase/throw_test.cpp` 添加到测试目标
   - 编译并运行测试，确保全部通过
   - _需求：全部_
