# xbase — TODO

> Planned optimizations and additions to the xbase module. Items are listed roughly in priority order.

## xTaskGroup — Work-Stealing Thread Pool

### Problem

The current `xTaskGroup` uses a single shared task queue protected by `pthread_mutex_t` (`qlock`). All workers contend on this lock when dequeuing tasks, and all submitters contend on it when enqueuing. Under high task throughput with many worker threads, `qlock` becomes a scalability bottleneck.

The lock cannot be replaced with `xMpsc` because the task queue is **MPMC** (multiple producers, multiple consumers), while `xMpsc` only supports single-consumer access.

### Proposed Solution — Work-Stealing

Each worker thread owns a **local task deque** (double-ended queue). Submitters distribute tasks to worker deques via round-robin or least-loaded selection. Workers pop from their own deque (LIFO, cache-friendly); when a worker's deque is empty, it **steals** from another worker's deque (FIFO, fairness).

```c
Submitter ──round-robin──▶ Worker 0 deque ◀──steal── Worker 1
                           Worker 1 deque ◀──steal── Worker 2
                           Worker 2 deque ◀──steal── Worker 0
```

### Key Design Points

| Aspect | Detail |
| --- | --- |
| **Local deque** | Chase-Lev work-stealing deque — lock-free for owner push/pop, CAS-based for stealer |
| **Task distribution** | Round-robin with `atomic_fetch_add` on a shared counter |
| **Steal policy** | Random victim selection to avoid thundering herd |
| **Idle wait** | Per-worker `xNote` or eventfd; submitter signals the target worker |
| **Fallback** | If all deques are full, fall back to a shared overflow queue (current `qlock`-based queue) |

### Benefits

- Eliminates the single `qlock` bottleneck — workers rarely contend with each other
- LIFO local execution improves cache locality (recently submitted tasks are hot)
- Stealing provides automatic load balancing without centralized scheduling

### Complexity

High. Requires a correct Chase-Lev deque implementation with careful memory ordering, plus steal-half vs steal-one policy tuning. Recommended as a future optimization when profiling shows `qlock` contention is a real bottleneck.

### Priority

P2 — The current single-queue design is adequate for typical workloads (event-loop offload with moderate worker counts). The TLS freelist and `xNote`-based completion already address the main hot paths. Revisit when benchmarks show lock contention under high core counts (≥32 threads).
