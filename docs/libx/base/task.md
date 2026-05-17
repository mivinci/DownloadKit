# task.h — N:M Task Model

## Introduction

`task.h` provides a lightweight N:M concurrent task model where N user tasks are multiplexed onto M OS threads managed by a task group (thread pool). It supports lazy thread creation, configurable queue capacity, per-task result retrieval, and a global shared task group for convenience.

## Design Philosophy

1. **Lazy Thread Spawning** — Worker threads are created on-demand when tasks are submitted and no idle thread is available, up to the configured maximum. This avoids pre-allocating threads that may never be used, reducing resource consumption for bursty workloads.

2. **Simple Submit/Wait Model** — Tasks are submitted with `xTaskSubmit()` and optionally awaited with `xTaskWait()`. This mirrors the future/promise pattern found in higher-level languages, but in pure C with minimal overhead.

3. **Safe Cancellation** — `xTaskCancel()` uses a single CAS (compare-and-swap) to atomically transition a queued task to the cancelled state. If the task is still in the queue, the cancel succeeds and the caller can safely release the task's argument. If the task is already running or done, the cancel fails and the caller must `xTaskWait()` first.

4. **Configurable Capacity** — The task group can be configured with a maximum thread count and queue capacity. When the queue is full, `xTaskSubmit()` returns NULL, giving the caller explicit backpressure.

5. **Global Shared Group** — `xTaskGroupGlobal()` provides a lazily-initialized, process-wide task group with default settings (unlimited threads, no queue cap). It's automatically destroyed at `atexit()`, making it convenient for fire-and-forget usage.

## Architecture

```mermaid
graph TD
    subgraph "Task Group"
        QUEUE["Task Queue (FIFO)"]
        W1["Worker Thread 1"]
        W2["Worker Thread 2"]
        WN["Worker Thread N"]
    end

    APP["Application"] -->|"xTaskSubmit()"| QUEUE
    QUEUE -->|"dequeue"| W1
    QUEUE -->|"dequeue"| W2
    QUEUE -->|"dequeue"| WN

    W1 -->|"done"| RESULT["xTaskWait() → result"]
    W2 -->|"done"| RESULT
    WN -->|"done"| RESULT

    style APP fill:#4a90d9,color:#fff
    style QUEUE fill:#f5a623,color:#fff
    style RESULT fill:#50b86c,color:#fff
```

## Implementation Details

### Internal Structure

```c
struct xTask_ {
    xTaskFunc       fn;       // User function
    void           *arg;      // User argument
    xNote           note;     // 4-byte one-shot completion notification
    void           *result;   // Return value of fn
    struct xTaskGroup_ *group; // Back-pointer to owning group
    struct xTask_  *next;     // Intrusive queue linkage (task queue + TLS freelist)
    xMpsc           done_link; // Lock-free done-list linkage (xMpsc)
    atomic_int      state;    // QUEUED → RUNNING/CANCELLED → DONE (CAS-based cancel)
};
// sizeof(xTask_) ≈ 48 bytes (down from ~136 bytes with mutex+cond)

struct xTaskGroup_ {
    pthread_t      *workers;      // Dynamic array of worker threads
    size_t          max_threads;  // Upper bound (SIZE_MAX if unlimited)
    size_t          nthreads;     // Currently spawned threads
    pthread_mutex_t qlock;        // Protects the task queue
    pthread_cond_t  qcond;        // Wakes idle workers
    struct xTask_  *qhead, *qtail; // FIFO task queue
    size_t          qsize, qcap;  // Current size and capacity
    xMpsc          *done_head;    // Lock-free MPSC done queue (head)
    xMpsc          *done_tail;    // Lock-free MPSC done queue (tail)
    size_t          idle;         // Number of idle workers
    atomic_size_t   pending;      // Submitted - finished
    atomic_size_t   done_count;   // Tasks completed
    pthread_cond_t  wcond;        // Dedicated cond for xTaskGroupWait()
    bool            shutdown;     // Shutdown flag
};
```

### TLS Freelist

In the common event-loop offload path, `xTaskSubmit()` (alloc) and `xTaskWait()` (free) happen on the same thread. A per-thread freelist eliminates `malloc`/`free` overhead entirely — zero locks, zero atomics. The `task->next` pointer is reused as the freelist link (zero extra memory). A per-thread cap of 64 prevents unbounded caching.

```c
static __thread struct {
    struct xTask_ *head;
    size_t         count;
} tl_free = {NULL, 0};
```

### Worker Loop

Each worker thread runs `worker_loop()`:

1. **Acquire lock** and increment `idle` count.
2. **Wait** on `qcond` while the queue is empty and not shutting down.
3. **Dequeue** one task, decrement `idle`.
4. **CAS state QUEUED → RUNNING** — if the CAS fails (task was cancelled), skip execution.
5. **Execute** `task->fn(task->arg)` (only if step 4 succeeded).
6. **Push to done queue** via `xMpscPush()` (lock-free, wait-free for producers).
7. **Signal completion** via `xNoteSignal()` (atomic store + kernel wake).
8. **Update counters** — decrement `pending`, signal `wcond` if all tasks are done.

### Task Submission Flow

```mermaid
flowchart TD
    SUBMIT["xTaskSubmit(group, fn, arg)"]
    CHECK_CAP{"Queue full?"}
    ENQUEUE["Enqueue task"]
    CHECK_IDLE{"Idle workers > 0?"}
    SIGNAL["Signal qcond"]
    CHECK_MAX{"nthreads < max?"}
    SPAWN["Spawn new worker"]
    DONE["Return task handle"]
    FAIL["Return NULL"]

    SUBMIT --> CHECK_CAP
    CHECK_CAP -->|Yes| FAIL
    CHECK_CAP -->|No| ENQUEUE
    ENQUEUE --> CHECK_IDLE
    CHECK_IDLE -->|Yes| SIGNAL
    CHECK_IDLE -->|No| CHECK_MAX
    CHECK_MAX -->|Yes| SPAWN
    CHECK_MAX -->|No| DONE
    SPAWN --> SIGNAL
    SIGNAL --> DONE

    style SUBMIT fill:#4a90d9,color:#fff
    style FAIL fill:#e74c3c,color:#fff
    style DONE fill:#50b86c,color:#fff
```

### Separate Wait Conditions

The implementation uses two separate condition variables:

- `qcond` — Wakes idle workers when a new task arrives.
- `wcond` — Wakes `xTaskGroupWait()` callers when all tasks complete.

Using a single condition variable caused lost wakeups: `pthread_cond_signal()` could wake an idle worker instead of the `GroupWait` caller, leaving it blocked forever.

### Global Task Group

`xTaskGroupGlobal()` uses `pthread_once` for thread-safe lazy initialization. The group is registered with `atexit()` for automatic cleanup. It uses default configuration (unlimited threads, no queue cap).

## API Reference

### Types

| Type | Description |
| --- | --- |
| `xTaskFunc` | `void *(*)(void *arg)` — Task function signature. Returns a result pointer. |
| `xTask` | Opaque handle to a submitted task |
| `xTaskGroup` | Opaque handle to a task group (thread pool) |
| `xTaskGroupConf` | Configuration struct: `nthreads` (0 = auto), `queue_cap` (0 = unbounded) |

### Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xTaskGroupCreate` | `xTaskGroup xTaskGroupCreate(const xTaskGroupConf *conf)` | Create a task group. NULL conf = defaults. | Not thread-safe |
| `xTaskGroupDestroy` | `void xTaskGroupDestroy(xTaskGroup g)` | Wait for pending tasks, then destroy. | Not thread-safe |
| `xTaskSubmit` | `xTask xTaskSubmit(xTaskGroup g, xTaskFunc fn, void *arg)` | Submit a task. Returns NULL if queue is full. | **Thread-safe** |
| `xTaskWait` | `xErrno xTaskWait(xTask t, void **result)` | Block until task completes. Returns `xErrno_Cancelled` if the task was cancelled. | **Thread-safe** |
| `xTaskCancel` | `xErrno xTaskCancel(xTask t)` | Cancel a queued task. Returns `xErrno_Ok` on success, `xErrno_Busy` if already running/done. | **Thread-safe** |
| `xTaskGroupWait` | `xErrno xTaskGroupWait(xTaskGroup g)` | Block until all pending tasks complete. | **Thread-safe** |
| `xTaskGroupThreads` | `size_t xTaskGroupThreads(xTaskGroup g)` | Return number of spawned worker threads. | **Thread-safe** (atomic read) |
| `xTaskGroupPending` | `size_t xTaskGroupPending(xTaskGroup g)` | Return number of pending tasks. | **Thread-safe** (atomic read) |
| `xTaskGroupGlobal` | `xTaskGroup xTaskGroupGlobal(void)` | Get the global shared task group (lazy init). | **Thread-safe** |

## Usage Examples

### Basic Task Submission

```c
#include <stdio.h>
#include <x/base/task.h>

static void *compute(void *arg) {
    int *val = (int *)arg;
    *val *= 2;
    return val;
}

int main(void) {
    xTaskGroup group = xTaskGroupCreate(NULL);

    int value = 21;
    xTask task = xTaskSubmit(group, compute, &value);

    void *result;
    xTaskWait(task, &result);
    printf("Result: %d\n", *(int *)result); // 42

    xTaskGroupDestroy(group);
    return 0;
}
```

### Parallel Map

```c
#include <stdio.h>
#include <x/base/task.h>

#define N 8

static void *square(void *arg) {
    int *val = (int *)arg;
    *val = (*val) * (*val);
    return val;
}

int main(void) {
    xTaskGroupConf conf = { .nthreads = 4, .queue_cap = 0 };
    xTaskGroup group = xTaskGroupCreate(&conf);

    int data[N] = {1, 2, 3, 4, 5, 6, 7, 8};
    xTask tasks[N];

    for (int i = 0; i < N; i++)
        tasks[i] = xTaskSubmit(group, square, &data[i]);

    // Wait for all
    xTaskGroupWait(group);

    for (int i = 0; i < N; i++)
        printf("data[%d] = %d\n", i, data[i]);

    // Clean up task handles
    for (int i = 0; i < N; i++)
        xTaskWait(tasks[i], NULL);

    xTaskGroupDestroy(group);
    return 0;
}
```

### Cancelling a Task

```c
#include <stdio.h>
#include <stdlib.h>
#include <x/base/task.h>

static void *process(void *arg) {
    int *data = (int *)arg;
    printf("Processing: %d\n", *data);
    return NULL;
}

int main(void) {
    xTaskGroup group = xTaskGroupCreate(NULL);

    int *data = (int *)malloc(sizeof(int));
    *data = 42;
    xTask task = xTaskSubmit(group, process, data);

    // Try to cancel — if successful, we can safely free data now.
    if (xTaskCancel(task) == xErrno_Ok) {
        free(data);  // Safe: fn was never called
    } else {
        // Task is already running — must wait before freeing
        xTaskWait(task, NULL);
        free(data);
    }

    xTaskGroupDestroy(group);
    return 0;
}
```

### Using the Global Task Group

```c
#include <stdio.h>
#include <x/base/task.h>

static void *work(void *arg) {
    printf("Running on global pool: %s\n", (char *)arg);
    return NULL;
}

int main(void) {
    xTask t = xTaskSubmit(xTaskGroupGlobal(), work, "hello");
    xTaskWait(t, NULL);
    // No need to destroy the global group
    return 0;
}
```

## Use Cases

1. **CPU-Bound Parallel Processing** — Distribute computation across multiple cores. Use `xTaskGroupWait()` to synchronize at barriers.

2. **Event Loop Offload** — The event loop's `xEventLoopSubmit()` uses `xTaskGroup` internally to run work functions on worker threads, then delivers results back to the loop thread.

3. **Background I/O** — Offload blocking file I/O (e.g., `fsync`, large reads) to a thread pool to keep the main thread responsive.

## Best Practices

- **Always call `xTaskWait()` or let `xTaskGroupDestroy()` clean up.** Each `xTaskSubmit()` allocates a task struct (from the TLS freelist or malloc). Task memory is reclaimed when the done queue is drained (during `xTaskGroupWait()` or `xTaskGroupDestroy()`). Leaking task handles leaks resources.
- **Check `xTaskCancel()` return value before releasing the arg.** `xErrno_Ok` means the task will not execute — safe to free. `xErrno_Busy` means it's already running or done — you must `xTaskWait()` first.
- **Set `queue_cap` for backpressure.** Without a cap, unbounded submission can exhaust memory. A bounded queue lets you detect overload via NULL returns from `xTaskSubmit()`.
- **Don't destroy the global group.** `xTaskGroupGlobal()` is managed internally and destroyed at `atexit()`. Passing it to `xTaskGroupDestroy()` is undefined behavior.
- **Use `xTaskGroupWait()` for barriers, not busy-polling.** It uses a dedicated condition variable and blocks efficiently.

## Comparison with Other Libraries

| Feature | xbase task.h | pthread | C11 threads | GCD (libdispatch) |
| --- | --- | --- | --- | --- |
| **Abstraction** | Task (submit/wait) | Thread (create/join) | Thread (create/join) | Block (dispatch_async) |
| **Thread Management** | Automatic (lazy spawn) | Manual | Manual | Automatic |
| **Queue** | Built-in FIFO with cap | N/A | N/A | Built-in (serial/concurrent) |
| **Result Retrieval** | `xTaskWait(t, &result)` | `pthread_join(t, &result)` | `thrd_join(t, &result)` | Completion handler |
| **Group Wait** | `xTaskGroupWait()` | Manual barrier | Manual barrier | `dispatch_group_wait()` |
| **Backpressure** | `queue_cap` → NULL on full | N/A | N/A | N/A (unbounded) |
| **Global Pool** | `xTaskGroupGlobal()` | N/A | N/A | `dispatch_get_global_queue()` |
| **Platform** | macOS + Linux | POSIX | C11 | macOS + Linux (via libdispatch) |
| **Dependencies** | pthread | OS | OS | OS / libdispatch |

**Key Differentiator:** xbase's task model provides a simple, portable thread pool with lazy spawning and explicit backpressure — features that require significant boilerplate with raw pthreads. Unlike GCD, it gives you direct control over thread count and queue capacity.
