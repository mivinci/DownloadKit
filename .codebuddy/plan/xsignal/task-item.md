# 实施计划

- [ ] 1. 在 `event.h` 中声明信号 API
   - 在 `xEventTimerFunc` typedef 附近添加 `typedef void (*xEventSignalFunc)(int signo, void *arg)`
   - 在 `xEventLoopStop` 声明之后添加 `xEventLoopSignalWatch` 函数声明及 Doxygen 注释
   - _需求：1.1、1.4、1.5、2.1_

- [ ] 2. 在 `event_base.h` 中扩展信号监视数据结构
   - 定义 `XK_SIGNAL_MAX`（如 64）和 `struct xSignalWatch_`（包含 `xEventSignalFunc fn` + `void *arg`）
   - 在 `struct xEventLoop_` 中添加 `struct xSignalWatch_ signal_watches[XK_SIGNAL_MAX]` 数组
   - _需求：7.3_

- [ ] 3. 实现 kqueue 后端信号支持（`event_kqueue.c`）
   - [ ] 3.1 实现 `xEventLoopSignalWatch` 函数
      - 参数校验（NULL loop、无效 signo、SIGKILL/SIGSTOP 返回 `xErrno_InvalidArg`）
      - `fn != NULL` 时：记录回调到 `signal_watches[signo]`，调用 `signal(signo, SIG_IGN)`，通过 `EV_SET` + `kevent` 注册 `EVFILT_SIGNAL`
      - `fn == NULL` 时：通过 `EV_DELETE` 移除 `EVFILT_SIGNAL`，调用 `signal(signo, SIG_DFL)` 恢复默认，清空 `signal_watches[signo]`
      - _需求：1.1、1.2、1.3、1.4、1.5、1.6、3.1、3.2、3.4、10.1_
   - [ ] 3.2 在 `xEventWait` 事件分发循环中增加 `EVFILT_SIGNAL` 分支
      - 在现有 `EVFILT_READ` / `EVFILT_WRITE` 判断之前，检查 `events[i].filter == EVFILT_SIGNAL`
      - 通过 `events[i].ident` 获取信号编号，查找 `signal_watches[signo]` 并调用回调
      - _需求：3.3、6.1、8.1、8.4_

- [ ] 4. 实现 epoll 后端信号支持（`event_epoll.c`）
   - [ ] 4.1 扩展 `struct xEventLoopEpoll_` 添加 signalfd 映射
      - 添加 `int signal_fds[XK_SIGNAL_MAX]` 数组（初始化为 -1），记录每个信号对应的 signalfd
      - 在 `xEventLoopCreate` 中初始化该数组，在 `xEventLoopDestroy` 中清理未关闭的 signalfd
      - _需求：7.3_
   - [ ] 4.2 实现 `xEventLoopSignalWatch` 函数
      - 参数校验同 kqueue 后端
      - `fn != NULL` 时：`sigprocmask` 阻塞信号，`signalfd` 创建 fd，`epoll_ctl` 注册为 `EPOLLIN`，记录回调和 fd
      - `fn == NULL` 时：`epoll_ctl` 移除，`close(signalfd)`，`sigprocmask` 解除阻塞，`signal(signo, SIG_DFL)`
      - 重复注册时仅替换回调和 arg，不重建 signalfd
      - _需求：1.1–1.6、4.1–4.5、10.1_
   - [ ] 4.3 在 `xEventWait` 事件分发循环中识别 signalfd 事件
      - signalfd 的 `data.ptr` 需要与普通 `xEventSource_` 区分（可用特殊标记或通过 fd 查找 `signal_fds` 数组）
      - 读取 `struct signalfd_siginfo`，提取 `ssi_signo`，调用对应回调
      - _需求：4.3、6.1、8.2、8.4_

- [ ] 5. 实现 poll 后端信号支持（`event_poll.c`）
   - [ ] 5.1 扩展 `struct xEventLoopPoll_` 添加 signal pipe 映射
      - 添加 `int signal_pipe_r[XK_SIGNAL_MAX]` 和 `int signal_pipe_w[XK_SIGNAL_MAX]` 数组（初始化为 -1）
      - 定义全局 `signal_pipe_w` 指针供信号处理函数使用
      - 在 `xEventLoopCreate` / `xEventLoopDestroy` 中初始化和清理
      - _需求：7.3_
   - [ ] 5.2 实现 `xEventLoopSignalWatch` 函数
      - 参数校验同上
      - `fn != NULL` 时：创建 pipe 对，保存写端到全局状态，`sigaction` 安装处理函数（仅写信号编号到 pipe），记录回调
      - `fn == NULL` 时：`sigaction` 恢复 `SIG_DFL`，关闭 pipe 两端，清空状态
      - _需求：1.1–1.6、5.1–5.4、10.1_
   - [ ] 5.3 在 `pfd_rebuild` 和 `xEventWait` 中集成 signal pipe
      - `pfd_rebuild` 中将活跃的 signal pipe 读端加入 `pollfds` 数组
      - `xEventWait` 中检查 signal pipe 读端的 `POLLIN`，读取信号编号并调用回调
      - _需求：5.3、6.1、8.3、8.4_

- [ ] 6. 在 `event_test.cpp` 中编写信号功能单元测试
   - [ ] 6.1 基本注册与触发测试：注册 `SIGUSR1`，`kill(getpid(), SIGUSR1)` 后验证回调被调用
   - [ ] 6.2 取消测试：注册后取消，再发信号验证回调不被调用
   - [ ] 6.3 替换测试：对同一信号重复注册，验证新回调替换旧回调
   - [ ] 6.4 参数校验测试：NULL loop、SIGKILL、SIGSTOP、负数 signo 返回 `xErrno_InvalidArg`
   - [ ] 6.5 多信号测试：同时监听 `SIGUSR1` + `SIGUSR2`，验证各自独立触发
   - [ ] 6.6 信号回调中 `xEventLoopStop` 测试：在回调中停止循环，验证 `xEventLoopRun` 正常返回
   - _需求：9.1–9.7、6.1、6.2_
