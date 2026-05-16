# event.h — Cross-Platform Event Loop

## Introduction

`event.h` provides a cross-platform, edge-triggered event loop abstraction for I/O multiplexing. It unifies three OS-specific backends — **kqueue** (macOS/BSD), **epoll** (Linux), and **poll** (POSIX fallback) — behind a single API. The event loop is the central coordination point in xbase: it monitors file descriptors for readiness, dispatches timer callbacks, offloads CPU-bound work to thread pools, and watches for POSIX signals — all from a single thread.

## Design Philosophy

1. **Edge-Triggered Everywhere** — All three backends operate in edge-triggered mode. kqueue uses `EV_CLEAR`, epoll uses `EPOLLET`, and poll emulates edge-triggered behavior by clearing the event mask after each notification (requiring the caller to re-arm via `xEventMod()`). This design encourages callers to drain fds completely, reducing spurious wakeups.

2. **Backend Selection at Compile Time** — The backend is chosen via preprocessor macros (`MOO_HAS_KQUEUE`, `MOO_HAS_EPOLL`), with poll as the universal fallback. This means zero runtime dispatch overhead.

3. **Integrated Timer Heap** — Rather than requiring a separate timer facility, the event loop embeds a min-heap of timer entries. `xEventWait()` automatically adjusts its timeout to fire the earliest timer, providing sub-millisecond timer resolution without a dedicated timer thread.

4. **Thread-Pool Offload** — `xEventLoopSubmit()` bridges the event loop and the task system: CPU-bound work runs on a worker thread, and the completion callback is dispatched on the event loop thread via a lock-free MPSC queue + cross-thread wake, ensuring single-threaded callback semantics. Offloaded work can be cancelled via `xEventLoopWorkCancel()` if it hasn't started yet.

6. **Direct Cross-Thread Posting** — `xEventLoopPost()` allows any thread to queue a callback for execution on the event loop thread without involving a thread pool. This is the lightest cross-thread communication primitive — ideal for notifying the loop of external events (e.g., ICE/TURN callbacks, inter-module signals) with zero thread-pool overhead.

5. **Self-Pipe Trick for Signals** — On epoll and poll backends, signal delivery uses the self-pipe trick (a `sigaction` handler writes to a pipe) rather than `signalfd`, avoiding the fragile requirement of blocking signals in every thread. On kqueue, `EVFILT_SIGNAL` is used natively.

## Architecture

```mermaid
graph TD
    subgraph "Event Loop (single thread)"
        WAIT["xEventWait()"]
        DISPATCH["Dispatch I/O callbacks"]
        TIMERS["Fire expired timers"]
        DONE["Drain done-queue"]
        SWEEP["Sweep deleted sources"]
    end

    subgraph "Backend (compile-time)"
        KQ["kqueue"]
        EP["epoll"]
        PO["poll"]
    end

    subgraph "Cross-Thread"
        WAKE["Wake (EVFILT_USER / eventfd / pipe)"]
        MPSC_Q["MPSC Done Queue"]
        WORKER["Worker Thread Pool"]
        POST["xEventLoopPost()"]
    end

    WAIT --> KQ
    WAIT --> EP
    WAIT --> PO
    KQ --> DISPATCH
    EP --> DISPATCH
    PO --> DISPATCH
    DISPATCH --> TIMERS
    TIMERS --> DONE
    DONE --> SWEEP

    WORKER -->|"push result"| MPSC_Q
    POST -->|"push callback"| MPSC_Q
    MPSC_Q -->|"wake"| WAKE
    WAKE -->|"drain"| DONE

    style WAIT fill:#4a90d9,color:#fff
    style DISPATCH fill:#4a90d9,color:#fff
    style TIMERS fill:#f5a623,color:#fff
    style DONE fill:#50b86c,color:#fff
```

### Event Loop Lifecycle

```mermaid
sequenceDiagram
    participant App
    participant EL as xEventLoop
    participant Backend as kqueue / epoll / poll
    participant Timer as Timer Heap

    App->>EL: xEventLoopCreate()
    App->>EL: xEventAdd(fd, mask, callback)
    App->>EL: xEventLoopTimerAfter(fn, 1000ms)
    App->>EL: xEventLoopRun()

    loop Main Loop
        EL->>Timer: Check earliest deadline
        Timer-->>EL: timeout = min(user_timeout, timer_deadline)
        EL->>Backend: wait(timeout)
        Backend-->>EL: ready events
        EL->>App: callback(fd, mask)
        EL->>Timer: Pop & fire expired timers
        EL->>EL: Sweep deleted sources
    end

    App->>EL: xEventLoopStop()
    App->>EL: xEventLoopDestroy()
```

## Implementation Details

### Backend Architecture

Each backend is implemented in a separate `.c` file that provides the full public API:

| File | Backend | Trigger Mode | Selection |
| --- | --- | --- | --- |
| `event_kqueue.c` | kqueue | `EV_CLEAR` (native edge) | `#ifdef MOO_HAS_KQUEUE` |
| `event_epoll.c` | epoll | `EPOLLET` (native edge) | `#ifdef MOO_HAS_EPOLL` |
| `event_poll.c` | poll(2) | Emulated edge (mask cleared after dispatch) | Fallback |

All backends share a common base structure (`struct xEventLoop_`) defined in `event_private.h`, which contains:

- A dynamic source array with deferred deletion (sweep after dispatch)
- A cross-thread wake mechanism (`EVFILT_USER` on kqueue, `eventfd` on epoll, pipe on poll) with atomic coalescing
- A min-heap for builtin timers (protected by `timer_mu` mutex)
- A lock-free MPSC done-queue for offload completion and posted callbacks
- Signal watch slots (up to `MOO_SIGNAL_MAX = 64`)

### Deferred Source Deletion

When `xEventDel()` is called during a callback dispatch, the source is marked `deleted = 1` rather than freed immediately. After the dispatch batch completes, `source_array_sweep()` frees all deleted sources. This prevents use-after-free when multiple events reference the same source in a single `xEventWait()` call.

### Cross-Thread Wake

Each backend uses the lightest available mechanism for cross-thread wakeup:

| Backend | Mechanism | Fds Used |
| --- | --- | --- |
| kqueue | `EVFILT_USER` with `NOTE_TRIGGER` | 0 (kernel event, no fd) |
| epoll | `eventfd` (`EFD_NONBLOCK \| EFD_CLOEXEC`) | 1 (`wake_rfd`) |
| poll | Non-blocking pipe (`wake_rfd` / `wake_wfd`) | 2 (POSIX fallback) |

`xEventWake()` triggers the backend-specific notification; the event loop drains it and processes the done-queue. Multiple wakes before the next `xEventWait()` are coalesced via an atomic `wake_pending` flag — only the first caller after the loop clears the flag performs the actual syscall, subsequent callers skip it entirely. This reduces wake overhead from O(N) syscalls to O(1) in batch completion scenarios.

### Timer Integration

Builtin timers are stored in a min-heap inside the event loop. Before each `xEventWait()` call, the effective timeout is clamped to the earliest timer deadline. After I/O dispatch, expired timers are popped and fired. Timer operations (`xEventLoopTimerAfter`, `xEventLoopTimerAt`, `xEventLoopTimerCancel`) are thread-safe, protected by `timer_mu`.

### Signal Handling

| Backend | Mechanism |
| --- | --- |
| kqueue | `EVFILT_SIGNAL` with `EV_CLEAR` — native kernel support |
| epoll | Self-pipe trick: `sigaction` handler writes to a per-signal pipe |
| poll | Self-pipe trick: same as epoll |

The self-pipe approach avoids `signalfd`'s requirement to block signals in all threads, which is fragile in the presence of third-party libraries and test frameworks.

## API Reference

### Types

| Type | Description |
| --- | --- |
| `xEventMask` | Bitmask enum: `xEvent_Read` (1), `xEvent_Write` (2), `xEvent_Timeout` (4) |
| `xEventFunc` | `void (*)(int fd, xEventMask mask, void *arg)` — I/O callback |
| `xEventTimerFunc` | `void (*)(void *arg)` — Timer callback |
| `xEventSignalFunc` | `void (*)(int signo, void *arg)` — Signal callback |
| `xEventDoneFunc` | `void (*)(void *arg, void *result)` — Offload completion callback |
| `xEventPostFunc` | `void (*)(void *arg)` — Posted callback (via `xEventLoopPost`) |
| `xEventLoop` | Opaque handle to an event loop |
| `xEventSource` | Opaque handle to a registered event source |
| `xEventTimer` | Opaque handle to a builtin timer |
| `xEventWork` | Opaque handle to a submitted offload work item |

### Functions

#### Lifecycle

| Function | Signature | Thread Safety |
| --- | --- | --- |
| `xEventLoopCreate` | `xEventLoop xEventLoopCreate(void)` | Not thread-safe |
| `xEventLoopCreateWithGroup` | `xEventLoop xEventLoopCreateWithGroup(xTaskGroup group)` | Not thread-safe |
| `xEventLoopDestroy` | `void xEventLoopDestroy(xEventLoop loop)` | Not thread-safe |
| `xEventLoopRun` | `void xEventLoopRun(xEventLoop loop)` | Not thread-safe (call from one thread) |
| `xEventLoopStop` | `void xEventLoopStop(xEventLoop loop)` | **Thread-safe** |
| `xEventLoopWait` | `xErrno xEventLoopWait(xEventLoop loop, int timeout_ms)` | Not thread-safe (call from one thread) |

#### I/O Sources

| Function | Signature | Thread Safety |
| --- | --- | --- |
| `xEventAdd` | `xEventSource xEventAdd(xEventLoop loop, int fd, xEventMask mask, xEventFunc fn, void *arg)` | Not thread-safe |
| `xEventMod` | `xErrno xEventMod(xEventLoop loop, xEventSource src, xEventMask mask)` | Not thread-safe |
| `xEventDel` | `xErrno xEventDel(xEventLoop loop, xEventSource src)` | Not thread-safe |
| `xEventWait` | `int xEventWait(xEventLoop loop, int timeout_ms)` | Not thread-safe |

#### Timers

| Function | Signature | Thread Safety |
| --- | --- | --- |
| `xEventLoopTimerAfter` | `xEventTimer xEventLoopTimerAfter(xEventLoop loop, xEventTimerFunc fn, void *arg, uint64_t delay_ms)` | **Thread-safe** |
| `xEventLoopTimerAt` | `xEventTimer xEventLoopTimerAt(xEventLoop loop, xEventTimerFunc fn, void *arg, uint64_t abs_ms)` | **Thread-safe** |
| `xEventLoopTimerCancel` | `xErrno xEventLoopTimerCancel(xEventLoop loop, xEventTimer timer)` | **Thread-safe** |

#### Cross-Thread

| Function | Signature | Thread Safety |
| --- | --- | --- |
| `xEventWake` | `xErrno xEventWake(xEventLoop loop)` | **Thread-safe** (signal-handler-safe) |
| `xEventLoopPost` | `xErrno xEventLoopPost(xEventLoop loop, xEventPostFunc fn, void *arg)` | **Thread-safe** |
| `xEventLoopSubmit` | `xErrno xEventLoopSubmit(xEventLoop loop, xTaskGroup group, xTaskFunc work_fn, xEventDoneFunc done_fn, void *arg, xEventWork *out)` | **Thread-safe** |
| `xEventLoopWorkCancel` | `xErrno xEventLoopWorkCancel(xEventLoop loop, xEventWork work)` | **Thread-safe** |

#### Signal

| Function | Signature | Thread Safety |
| --- | --- | --- |
| `xEventLoopSignalWatch` | `xErrno xEventLoopSignalWatch(xEventLoop loop, int signo, xEventSignalFunc fn, void *arg)` | Not thread-safe |

#### Deprecated

| Function | Signature | Replacement |
| --- | --- | --- |
| `xEventLoopNowMs` | `uint64_t xEventLoopNowMs(void)` | `xMonoMs()` from `<x/base/time.h>` |

## Usage Examples

### Basic Event Loop with Timer

```c
#include <stdio.h>
#include <x/base/event.h>

static void on_timer(void *arg) {
    printf("Timer fired!\n");
    xEventLoopStop((xEventLoop)arg);
}

int main(void) {
    xEventLoop loop = xEventLoopCreate();
    if (!loop) return 1;

    // Fire after 500ms
    xEventLoopTimerAfter(loop, on_timer, loop, 500);

    xEventLoopRun(loop);
    xEventLoopDestroy(loop);
    return 0;
}
```

### Monitoring a File Descriptor

```c
#include <stdio.h>
#include <unistd.h>
#include <x/base/event.h>

static void on_readable(int fd, xEventMask mask, void *arg) {
    char buf[1024];
    ssize_t n;
    // Edge-triggered: must drain completely
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
    }
    (void)mask;
    (void)arg;
}

int main(void) {
    xEventLoop loop = xEventLoopCreate();

    // Monitor stdin for readability
    xEventAdd(loop, STDIN_FILENO, xEvent_Read, on_readable, NULL);

    // Run for up to 10 seconds, then stop
    xEventLoopTimerAfter(loop, (xEventTimerFunc)xEventLoopStop, loop, 10000);
    xEventLoopRun(loop);

    xEventLoopDestroy(loop);
    return 0;
}
```

### Bounded Wait with Timeout

```c
#include <stdio.h>
#include <x/base/event.h>

static void on_done(void *arg) {
    printf("Work complete!\n");
    xEventLoopStop((xEventLoop)arg);
}

int main(void) {
    xEventLoop loop = xEventLoopCreate();

    xEventLoopTimerAfter(loop, on_done, loop, 500);

    // Wait up to 5 seconds — returns xErrno_Ok if stopped,
    // or xErrno_Timeout if the deadline expires.
    xErrno rc = xEventLoopWait(loop, 5000);
    if (rc == xErrno_Timeout) {
        printf("Timed out!\n");
    }

    xEventLoopDestroy(loop);
    return 0;
}
```

### Posting a Callback to the Loop Thread

```c
#include <stdio.h>
#include <pthread.h>
#include <x/base/event.h>

static void on_notify(void *arg) {
    // Runs on the event loop thread — safe to access loop state
    printf("Notified from another thread!\n");
    xEventLoopStop((xEventLoop)arg);
}

static void *background_thread(void *arg) {
    xEventLoop loop = (xEventLoop)arg;
    // Do some work...
    xEventLoopPost(loop, on_notify, loop);
    return NULL;
}

int main(void) {
    xEventLoop loop = xEventLoopCreate();

    pthread_t th;
    pthread_create(&th, NULL, background_thread, loop);

    xEventLoopRun(loop);

    pthread_join(th, NULL);
    xEventLoopDestroy(loop);
    return 0;
}
```

### Offloading Work to a Thread Pool

```c
#include <stdio.h>
#include <x/base/event.h>

static void *heavy_work(void *arg) {
    // Runs on a worker thread
    int *val = (int *)arg;
    *val *= 2;
    return val;
}

static void on_done(void *arg, void *result) {
    // Runs on the event loop thread
    int *val = (int *)result;
    printf("Result: %d\n", *val);
    (void)arg;
}

int main(void) {
    xEventLoop loop = xEventLoopCreate();
    int value = 21;

    xEventLoopSubmit(loop, NULL, heavy_work, on_done, &value, NULL);

    // Run briefly to process the completion
    xEventLoopTimerAfter(loop, (xEventTimerFunc)xEventLoopStop, loop, 1000);
    xEventLoopRun(loop);

    xEventLoopDestroy(loop);
    return 0;
}
```

## Use Cases

1. **Network Servers** — Register listening sockets and accepted connections with the event loop. Use edge-triggered callbacks to read/write data without blocking. Combine with `xSocket` for idle-timeout support.

2. **Timer-Driven State Machines** — Use `xEventLoopTimerAfter()` to schedule state transitions, retries, or heartbeat checks. The timer is integrated into the event loop, so no separate timer thread is needed.

3. **Hybrid I/O + CPU Workloads** — Use `xEventLoopSubmit()` to offload CPU-intensive parsing or compression to a thread pool, then process results on the event loop thread where I/O state is safely accessible. Use `xEventLoopWorkCancel()` to cancel pending work when the associated resource is being released.

4. **Cross-Thread Notifications** — Use `xEventLoopPost()` to notify the event loop from external callbacks (e.g., ICE/TURN completions, OS notifications) without the overhead of a thread pool round-trip. The callback runs on the loop thread, so no additional synchronisation is needed.

## Best Practices

- **Always drain fds in edge-triggered mode.** Read/write until `EAGAIN` in every callback. Missing data means you won't be notified again until new data arrives.
- **Never block in callbacks.** The event loop is single-threaded; a blocking call stalls all I/O and timer processing. Offload heavy work via `xEventLoopSubmit()`.
- **Prefer `xEventLoopPost()` over `xEventLoopSubmit()` when no worker thread is needed.** If you just need to run a callback on the loop thread from another thread, `xEventLoopPost()` avoids the thread-pool overhead entirely.
- **Use `xEventLoopRun()` for the main loop.** It handles timer dispatch and stop-flag checking automatically. Only use `xEventWait()` directly if you need custom loop logic. For tests or scenarios where you need a bounded wait, use `xEventLoopWait(loop, timeout_ms)` — it returns `xErrno_Ok` when stopped, or `xErrno_Timeout` if the deadline expires.
- **Cancel offloaded work when releasing resources.** If you submit work via `xEventLoopSubmit()` and the associated resource (passed as `arg`) is about to be freed, use `xEventLoopWorkCancel()` to prevent use-after-free. If cancel succeeds (`xErrno_Ok`), the arg is safe to free immediately. If it fails (`xErrno_InvalidState`), the work is already running — let `done_fn` handle cleanup.
- **Cancel timers you no longer need.** Uncancelled timers hold memory until they fire. Use `xEventLoopTimerCancel()` to free them early.
- **Be aware of the poll backend's edge emulation.** On systems without kqueue or epoll, the poll backend clears the event mask after dispatch. You must call `xEventMod()` to re-arm.

## Comparison with Other Libraries

| Feature | xbase event.h | libevent | libev | libuv |
| --- | --- | --- | --- | --- |
| **Trigger Mode** | Edge-triggered only | Level (default), edge optional | Level + edge | Level-triggered |
| **Backends** | kqueue, epoll, poll | kqueue, epoll, poll, select, devpoll, IOCP | kqueue, epoll, poll, select, port | kqueue, epoll, poll, IOCP |
| **Timer Integration** | Built-in min-heap | Separate timer API | Built-in | Built-in |
| **Thread Pool** | Built-in (`xEventLoopSubmit`) | None (external) | None (external) | Built-in (`uv_queue_work`) |
| **Signal Handling** | Self-pipe / EVFILT_SIGNAL | evsignal | ev_signal | uv_signal |
| **API Style** | Opaque handles, C99 | Struct-based, C89 | Struct-based, C89 | Handle-based, C99 |
| **Binary Size** | ~15 KB | ~200 KB | ~50 KB | ~500 KB |
| **Dependencies** | None | None | None | None |
| **Windows Support** | Not yet | Yes (IOCP) | Yes (select) | Yes (IOCP) |
| **Design Goal** | Minimal building block | Full-featured framework | Minimal + performant | Cross-platform framework |

**Key Differentiator:** xbase's event loop is intentionally minimal — it provides the essential primitives (I/O, timers, signals, thread-pool offload) without buffered I/O, DNS resolution, or HTTP parsing. This makes it ideal as a foundation layer for higher-level libraries (like xhttp) rather than a standalone application framework.

## Benchmark

> Environment: Apple M3 Pro, 36 GB RAM, macOS 26.4, Release build (`-O2`), kqueue backend.
> Source: [`xbase/event_bench.cpp`](https://github.com/mivinci/moo/blob/main/xbase/event_bench.cpp)
> Full report: [`docs/bench/event_loop.md`](../bench/event_loop.md)

### Core Operations

| Benchmark | Time (ns) | CPU (ns) | Iterations |
| --- | ---: | ---: | ---: |
| `BM_EventLoop_CreateDestroy` | 700 | 700 | 974,157 |
| `BM_EventLoop_WakeLatency` | 413 | 413 | 1,717,088 |
| `BM_EventLoop_PipeAddDel` | 1,144 | 1,144 | 612,118 |

- **Create/Destroy** takes ~700ns — reduced from ~2.8µs after eliminating the wake pipe (no more `pipe()` + two extra fds).
- **Wake latency** is ~413ns per wake+wait cycle via `EVFILT_USER`, down from ~879ns with the old pipe mechanism — a **2.1× improvement**.

### libuv Baseline Comparison

| Dimension | moo | libuv | Ratio |
| --- | ---: | ---: | ---: |
| **Wake Latency** | 413 ns | 417 ns | **Tied** (moo 1.01× faster) |
| **Timer (single)** | 461 ns | 1,517 ns | **moo 3.3× faster** |
| **Timer (×1000)** | 43,545 ns | 68,659 ns | **moo 1.6× faster** |
| **Offload (single)** | 3,785 ns | 3,449 ns | libuv 1.1× faster (tied) |
| **Offload (×1000)** | 456,426 ns | 218,513 ns | libuv 2.1× faster |

**Key Observations:**

- **Wake latency** — Now effectively tied with libuv (413ns vs 417ns) after switching to `EVFILT_USER` (kqueue) / `eventfd` (epoll) + atomic wake coalescing. Previously 2.1× slower.
- **Timer** — moo now **wins across all batch sizes** thanks to batch-pop with single lock acquisition and timer struct freelist pooling. Previously libuv was 4–5× faster at batch sizes.
- **Offload round-trip** — libuv remains ~2× faster at scale. The gap has narrowed at small batch sizes thanks to wake coalescing and work item pooling.
