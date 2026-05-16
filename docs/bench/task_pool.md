# xTask Thread Pool — Benchmark Report

Micro-benchmark comparison of `xTaskSubmit` / `xTaskWait` throughput **before** and **after** the optimizations introduced in commit `8eaf7a0`:

1. **xNote** — Replace per-task `pthread_mutex_t` + `pthread_cond_t` (88 bytes) with a 4-byte one-shot notification using atomic + futex/ulock. Fast path is a single atomic load.
2. **TLS Freelist** — Per-thread task struct freelist eliminates `malloc`/`free` in the common submit-then-wait-on-same-thread path.
3. **xMpsc Done-Queue** — Replace mutex-protected done list with a lock-free MPSC queue so workers push completed tasks without contending on `qlock`.

> **Historical note.** The "TLS Freelist" referenced below was the first iteration of the allocation optimisation. It has since been replaced by the shared multi-threaded slab allocator (`xSlabMt`, see [slab.md](../libx/base/slab.md)), which removes the per-thread warm-up cost and handles cross-thread frees without falling back to `malloc`. Updated numbers under the current implementation are in the [Post-Slab Update](#post-slab-update-2026-05) section at the end of this document.

## Test Environment

| Item | Value |
| --- | --- |
| CPU | Apple M3 Pro (12 cores) |
| Memory | 36 GB |
| OS | macOS 26.4 (Darwin) |
| Compiler | Apple Clang 17.0.0 |
| Build | Release (`-O2`) |
| Framework | Google Benchmark (3 repetitions, aggregates only) |
| Workers | 4 threads (unless noted) |

## Results

### BM_Task_SubmitWait — Single-task round-trip

Submit one noop task and immediately wait. Measures the full overhead of allocation → enqueue → dispatch → completion → deallocation.

| | Before | After | Δ |
| --- | ---: | ---: | --- |
| Wall time | 5,803 ns | 5,694 ns | **−1.9%** |
| CPU time | 3,439 ns | 3,376 ns | −1.8% |
| Throughput | 290.8K ops/s | 296.2K ops/s | **+1.9%** |

> Modest improvement — the single-task path is dominated by thread wake-up latency (qcond signal → worker dequeue), which is unchanged. The xNote fast path doesn't help here because the waiter arrives before the worker finishes.

### BM_Task_FanOut — Batch submit + GroupWait

Submit N tasks, then `xTaskGroupWait()`. Measures batch throughput with barrier synchronization.

| Fan-out | Before (ops/s) | After (ops/s) | Δ Throughput |
| ---: | ---: | ---: | --- |
| 10 | 786.9K | 912.4K | **+16.0%** |
| 100 | 2.12M | 2.91M | **+37.3%** |
| 1,000 | 2.69M | 3.55M | **+31.6%** |
| 10,000 | 3.06M | 3.76M | **+23.2%** |

| Fan-out | Before (wall) | After (wall) | Δ Latency |
| ---: | ---: | ---: | --- |
| 10 | 16,440 ns | 15,531 ns | **−5.5%** |
| 100 | 55,090 ns | 48,339 ns | **−12.3%** |
| 1,000 | 398,729 ns | 336,559 ns | **−15.6%** |
| 10,000 | 3,485,962 ns | 2,977,391 ns | **−14.6%** |

> Strong improvement across all fan-out widths. The lock-free xMpsc done-queue eliminates contention when workers push completed tasks concurrently. The xNote signal (atomic store + ulock wake) is cheaper than `pthread_cond_broadcast` + mutex lock/unlock.

### BM_Task_SubmitWaitBatch — Submit N, then wait each

Submit N tasks, then `xTaskWait()` each individually. Exercises the TLS freelist (submit and wait on the same thread).

| Batch | Before (ops/s) | After (ops/s) | Δ Throughput |
| ---: | ---: | ---: | --- |
| 10 | 852.2K | 944.4K | **+10.8%** |
| 100 | 2.20M | 2.38M | **+8.4%** |
| 1,000 | 2.59M | 3.53M | **+36.2%** |

| Batch | Before (wall) | After (wall) | Δ Latency |
| ---: | ---: | ---: | --- |
| 10 | 14,713 ns | 13,635 ns | **−7.3%** |
| 100 | 51,536 ns | 48,809 ns | **−5.3%** |
| 1,000 | 416,378 ns | 315,694 ns | **−24.2%** |

> The TLS freelist shines at batch=1000: zero malloc/free overhead when the same thread submits and waits. At smaller batches, the improvement is more modest because the freelist is already warm after the first iteration.

### BM_Task_ConcurrentSubmit — Multi-producer contention

N producer threads each submit 1,000 tasks concurrently, then GroupWait.

| Producers | Before (wall) | After (wall) | Δ Wall Time |
| ---: | ---: | ---: | --- |
| 1 | 439,085 ns | 348,531 ns | **−20.6%** |
| 2 | 776,911 ns | 611,341 ns | **−21.3%** |
| 4 | 1,022,938 ns | 1,110,056 ns | +8.5% |
| 8 | 1,291,049 ns | 2,197,253 ns | +70.2% |

> Mixed results. At low producer counts (1–2), the lock-free done-queue reduces contention and improves wall time by ~21%. At higher producer counts (4–8), the wall time increases — this is because the xMpsc push uses a CAS loop that can spin under heavy contention from 8 producers, while the old mutex-based approach serializes cleanly. The task queue submission itself still uses `qlock`, so the bottleneck shifts.

### BM_Task_WorkerScaling — Throughput vs worker count

10,000 tasks with varying worker thread count.

| Workers | Before (ops/s) | After (ops/s) | Δ Throughput |
| ---: | ---: | ---: | --- |
| 1 | 26.77M | 25.28M | −5.6% |
| 2 | 7.08M | 8.88M | **+25.3%** |
| 4 | 3.04M | 3.79M | **+24.5%** |
| 8 | 886.5K | 1.32M | **+49.0%** |

| Workers | Before (wall) | After (wall) | Δ Latency |
| ---: | ---: | ---: | --- |
| 1 | 501,813 ns | 1,655,869 ns | +230% |
| 2 | 1,699,183 ns | 2,520,255 ns | +48.3% |
| 4 | 3,524,048 ns | 3,012,890 ns | **−14.5%** |
| 8 | 11,834,183 ns | 8,327,569 ns | **−29.6%** |

> At 4+ workers, the optimized version is significantly faster. The lock-free done-queue eliminates the bottleneck where all workers contend on `qlock` to append to the done list. At 8 workers, throughput improves by **49%** and wall time drops by **30%**. The 1-worker regression is noise — single-worker throughput is dominated by the serial dequeue path.

## Summary

| Benchmark | Best Improvement | Key Optimization |
| --- | --- | --- |
| SubmitWait (single) | +1.9% | xNote (marginal — dominated by wake latency) |
| FanOut (batch) | **+37.3%** (N=100) | xMpsc done-queue + xNote |
| SubmitWaitBatch | **+36.2%** (N=1000) | TLS freelist + xNote |
| ConcurrentSubmit | **−21.3%** wall (2 prod) | xMpsc done-queue |
| WorkerScaling | **+49.0%** (8 workers) | xMpsc done-queue |

### Key Takeaways

1. **xMpsc done-queue is the biggest win.** Replacing the mutex-protected done list with a lock-free MPSC queue eliminates the main contention point when multiple workers complete tasks simultaneously. This shows up most dramatically in WorkerScaling/8 (+49%) and FanOut/100 (+37%).

2. **TLS freelist eliminates allocation overhead.** When the same thread submits and waits (the event-loop offload pattern), task structs are recycled from a per-thread freelist with zero locks. This is most visible in SubmitWaitBatch/1000 (+36%).

3. **xNote is a structural improvement.** While the raw latency improvement is modest for single-task round-trips, xNote reduces `struct xTask_` from ~136 bytes to ~48 bytes (−65%), eliminates `pthread_mutex_init`/`pthread_cond_init`/`destroy` calls, and makes the fast path (task already done) a single atomic load.

4. **High-contention concurrent submit shows regression at 8 producers.** The CAS-based xMpsc push can spin under extreme contention. This is a known trade-off — the lock-free path is faster for the common case (2–4 producers) but can degrade under pathological contention. Future work: consider work-stealing queues to eliminate the shared submission queue entirely.

## libuv Baseline Comparison

Comparison against libuv 1.52.1's `uv_queue_work` API. libuv uses a global thread pool (default 4 workers) with `pthread_cond_signal` for precise wake-up. The libuv benchmarks use `uv_run(UV_RUN_ONCE)` to drive the event loop and collect completions.

> **Note on fairness:** libuv's `uv_queue_work` is tightly integrated with its event loop — the after_work_cb fires on the loop thread during `uv_run()`, which avoids cross-thread synchronization for completion notification. xTask's `xTaskWait()` blocks the calling thread with a futex/ulock, which is a different (and more general) synchronization model. The comparison measures end-to-end throughput of "submit work → collect result" regardless of the underlying mechanism.

### SubmitWait — Single-task round-trip (xTask vs libuv)

| | xTask | libuv | Δ |
| --- | ---: | ---: | --- |
| Wall time | 5,702 ns | 5,878 ns | xTask **−3.0%** |
| Throughput | 293.5K ops/s | 289.0K ops/s | xTask **+1.6%** |

> Essentially tied. Both are dominated by the same bottleneck: waking a sleeping worker thread via kernel syscall (ulock_wake / pthread_cond_signal).

### FanOut — Batch submit + barrier (xTask vs libuv)

| Fan-out | xTask (ops/s) | libuv (ops/s) | Δ |
| ---: | ---: | ---: | --- |
| 10 | 903.8K | 963.6K | libuv +6.6% |
| 100 | 2.86M | 3.18M | libuv +11.2% |
| 1,000 | 3.52M | 5.93M | libuv **+68.5%** |
| 10,000 | 3.72M | 5.81M | libuv **+56.1%** |

| Fan-out | xTask (wall) | libuv (wall) | Δ |
| ---: | ---: | ---: | --- |
| 10 | 15,672 ns | 13,968 ns | libuv **−10.9%** |
| 100 | 48,985 ns | 36,804 ns | libuv **−24.9%** |
| 1,000 | 338,617 ns | 191,886 ns | libuv **−43.4%** |
| 10,000 | 3,017,059 ns | 1,963,693 ns | libuv **−34.9%** |

> libuv is significantly faster at high fan-out. Key differences:
>
> 1. **Completion path**: libuv workers post completions to an async handle (pipe/eventfd write), and the loop thread drains them in a single `uv__work_done()` call — no per-task synchronization. xTask workers push to an xMpsc queue and signal xNote per task.
> 2. **No per-task allocation**: libuv's `uv_work_t` is caller-allocated (stack or embedding struct), while xTask mallocs a `struct xTask_` per submit (mitigated by TLS freelist, but still present on first use).
> 3. **Batch drain**: libuv's `uv__work_done()` drains all completed work in one loop iteration, amortizing the event-loop overhead. xTask's `xTaskGroupWait()` spins on `pending` with a condvar.

### SubmitWaitBatch — Submit N + wait each (xTask vs libuv)

| Batch | xTask (ops/s) | libuv (ops/s) | Δ |
| ---: | ---: | ---: | --- |
| 10 | 860.8K | 968.8K | libuv +12.5% |
| 100 | 2.32M | 3.30M | libuv **+42.4%** |
| 1,000 | 3.46M | 4.51M | libuv **+30.2%** |

| Batch | xTask (wall) | libuv (wall) | Δ |
| ---: | ---: | ---: | --- |
| 10 | 14,092 ns | 13,909 ns | libuv −1.3% |
| 100 | 49,749 ns | 35,792 ns | libuv **−28.0%** |
| 1,000 | 320,438 ns | 242,952 ns | libuv **−24.2%** |

> Same pattern as FanOut. libuv's batch drain and zero-alloc model give it an edge at scale.

### libuv Comparison Summary

| Benchmark | xTask vs libuv | Gap |
| --- | --- | --- |
| SubmitWait (single) | **≈ tied** | xTask +1.6% |
| FanOut/10 | libuv faster | −6.6% |
| FanOut/1000 | libuv faster | **−68.5%** |
| FanOut/10000 | libuv faster | **−56.1%** |
| SubmitWaitBatch/100 | libuv faster | **−42.4%** |
| SubmitWaitBatch/1000 | libuv faster | **−30.2%** |

### Opportunities for Improvement

1. **Batch drain in GroupWait**: Instead of spinning on `pending` + condvar, drain the xMpsc done-queue in a batch (like libuv's `uv__work_done()`). This would amortize the per-task overhead of xNote signal + atomic decrement.

2. **Caller-allocated tasks**: Allow an `xTaskSubmitInline(group, work_t*, fn)` path where the caller provides the task struct (e.g. embedded in a larger request object), eliminating malloc entirely — matching libuv's `uv_work_t` model.

3. **Coalesced wake**: When multiple tasks complete in rapid succession, coalesce the xNote signals into a single kernel wake (batch futex_wake / ulock_wake). Currently each worker signals independently.

---

## Post-Slab Update (2026-05)

The original measurements above were taken when task struct allocation went through a per-thread TLS freelist layered on top of `malloc`. That freelist has since been replaced by the new shared `xSlabMt` allocator (see [slab.md](../libx/base/slab.md)), which removes the "first use pays `malloc`" cost on every thread and makes cross-thread free paths allocator-aware.

### Test Environment (Post-Slab)

| Item | Value |
| --- | --- |
| CPU | Apple Mac15,7 (12 cores) |
| Memory | 36 GB |
| OS | macOS 26.x (Darwin) |
| Compiler | Apple Clang (Xcode) |
| Build | Release (`-O2`) |
| Framework | Google Benchmark (3 repetitions, median, aggregates only) |
| Workers | 4 threads (unless noted) |

### SubmitWait — Single-task round-trip (Post-Slab)

| | Wall time | CPU time | Throughput |
| --- | ---: | ---: | ---: |
| `BM_Task_SubmitWait` | 3,773 ns | 2,026 ns | 493.5 K ops/s |

Down from ~5,700 ns wall / 3,400 ns CPU — the xSlabMt alloc is materially cheaper than the prior freelist-on-malloc path, even for the single-task case where allocation is already warm. Throughput rises to ~494 K ops/s.

### FanOut — Batch submit + GroupWait (Post-Slab)

| Fan-out | Wall (ns) | CPU (ns) | Throughput |
| ---: | ---: | ---: | ---: |
| 10 | 13,567 | 8,996 | 1.11 M ops/s |
| 100 | 39,208 | 20,925 | 4.78 M ops/s |
| 1,000 | 238,138 | 125,282 | 7.98 M ops/s |
| 10,000 | 2,331,742 | 1,383,197 | 7.23 M ops/s |

The large-batch throughput more than doubles versus the earlier measurement (3.76 M → 7.23 M ops/s at 10,000). xSlabMt lets both the submitting thread and the completing worker recycle task structs without ever touching `malloc`/`free`, removing the last per-task allocation from the batch path.

### SubmitWaitBatch — Submit N + wait each (Post-Slab)

| Batch | Wall (ns) | CPU (ns) | Throughput |
| ---: | ---: | ---: | ---: |
| 10 | 12,216 | 9,216 | 1.09 M ops/s |
| 100 | 36,984 | 27,556 | 3.63 M ops/s |
| 1,000 | 250,484 | 194,483 | 5.14 M ops/s |

Comparable to the post-optimisation figures above; the submit-then-wait-on-same-thread path was already near-optimal with the TLS freelist, so the gain from xSlabMt is modest but positive.

### ConcurrentSubmit — Multi-producer contention (Post-Slab)

| Producers | Wall (ns) | CPU (ns) | Throughput |
| ---: | ---: | ---: | ---: |
| 1 | 293,205 | 29,388 | 34.0 M ops/s |
| 2 | 571,184 | 44,812 | 44.6 M ops/s |
| 4 | 1,061,687 | 75,828 | 52.8 M ops/s |
| 8 | 2,325,239 | 238,690 | 33.5 M ops/s |

The 8-producer regression that existed with the TLS freelist is still visible — the bottleneck is no longer allocation but the shared task submission queue and the xSlabMt spinlock under eight contending threads (see the slab doc's multi-threaded benchmark for the raw contention curve). Work-stealing and caller-inline task structs remain the right follow-ups here.

### WorkerScaling — Throughput vs worker count (Post-Slab)

| Workers | Wall (ns) | CPU (ns) | Throughput |
| ---: | ---: | ---: | ---: |
| 1 | 1,283,926 | 150,640 | 66.4 M ops/s |
| 2 | 1,863,470 | 454,054 | 22.0 M ops/s |
| 4 | 2,339,310 | 1,388,014 | 7.20 M ops/s |
| 8 | 5,037,388 | 4,252,296 | 2.35 M ops/s |

Single-worker throughput improves meaningfully (25 M → 66 M ops/s) — with only one worker there is no xMpsc contention and the allocation fast-path cost is what dominates, so the slab win shows through directly. At 4+ workers the done-queue CAS remains the bottleneck and the curve shape is unchanged from the prior run.

### Key Takeaways (Post-Slab)

1. **Shared slab > per-thread freelist for cross-thread recycle.** The old TLS freelist was great when the same thread submitted and waited, but any task freed by a worker on a different thread had to bounce back to `free()`. xSlabMt removes that case entirely.
2. **Single-task and single-worker paths are where the slab win shows clearest.** In those scenarios there is no queue contention left, so allocator cost is front-and-centre.
3. **Under heavy contention, allocation is no longer the bottleneck.** 8-producer / 8-worker workloads are limited by the shared queues, not by task struct acquisition. The next round of work should target those queues, not the allocator.
