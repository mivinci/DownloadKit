# Event Loop — Benchmark Report

Micro-benchmark comparison of xKit's `xEventLoop` against libuv 1.52.1 across three dimensions: **cross-thread wake latency**, **timer scheduling**, and **offload round-trip** (submit work → done callback on loop thread).

## Test Environment

| Item | Value |
| --- | --- |
| CPU | Apple M3 Pro (12 cores) |
| Memory | 36 GB |
| OS | macOS 26.4 (Darwin) |
| Compiler | Apple Clang 17.0.0 |
| Build | Release (`-O2`) |
| Framework | Google Benchmark |
| Event Backend | kqueue (xKit), kqueue (libuv) |
| Workers | 4 threads (for offload benchmarks) |

## Results

### Core Operations (xKit only)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
| --- | ---: | ---: | ---: |
| `BM_EventLoop_CreateDestroy` | 2,773 | 2,773 | 235,725 |
| `BM_EventLoop_WakeLatency` | 879 | 879 | 786,676 |
| `BM_EventLoop_PipeAddDel` | 1,181 | 1,181 | 588,033 |

- **Create/Destroy** takes ~2.8µs — reflects kqueue fd creation + internal structure allocation. Acceptable for long-lived event loops.
- **Wake latency** is ~879ns per wake+wait cycle via the internal pipe mechanism.
- **Add/Del cycle** (register + unregister a pipe fd) takes ~1.2µs — low overhead for dynamic fd management.

### Wake Latency — xKit vs libuv

| | xKit | libuv | Ratio |
| --- | ---: | ---: | ---: |
| Time | 879 ns | 415 ns | 2.12× slower |

xKit uses a pipe-based wake mechanism (`write(wake_wfd)` → `read(wake_rfd)` + drain). libuv's `uv_async_t` is faster here — on macOS it uses a similar pipe internally but avoids the drain loop overhead by using a flag-based coalescing approach. The 2× gap suggests room for optimization in xKit's wake path (e.g., using `eventfd` on Linux, or a lighter drain strategy on macOS).

### Timer Scheduling

#### xKit

| Benchmark | Time (ns) | CPU (ns) | Throughput |
| --- | ---: | ---: | ---: |
| `BM_EventLoop_TimerSingle` | 974 | 974 | 1.03M items/s |
| `BM_EventLoop_TimerBatch/10` | 3,794 | 3,794 | 2.64M items/s |
| `BM_EventLoop_TimerBatch/100` | 31,483 | 31,479 | 3.18M items/s |
| `BM_EventLoop_TimerBatch/1000` | 318,881 | 318,805 | 3.14M items/s |

#### libuv

| Benchmark | Time (ns) | CPU (ns) | Throughput |
| --- | ---: | ---: | ---: |
| `BM_Libuv_TimerSingle` | 12,331 | 1,525 | 655.6k items/s |
| `BM_Libuv_TimerBatch/10` | 12,656 | 1,836 | 5.45M items/s |
| `BM_Libuv_TimerBatch/100` | 17,192 | 6,037 | 16.56M items/s |
| `BM_Libuv_TimerBatch/1000` | 84,568 | 73,537 | 13.60M items/s |

#### Comparison (CPU time)

| Batch Size | xKit (CPU ns) | libuv (CPU ns) | Ratio |
| ---: | ---: | ---: | ---: |
| 1 | 974 | 1,525 | **xKit 1.57× faster** |
| 10 | 3,794 | 1,836 | libuv 2.07× faster |
| 100 | 31,479 | 6,037 | libuv 5.21× faster |
| 1,000 | 318,805 | 73,537 | libuv 4.33× faster |

**Analysis:**

- **Single timer** — xKit wins at ~974ns vs libuv's ~1.5µs. xKit's timer path is simpler: heap push + `xEventWait` pops and fires in one call. libuv's `uv_timer_start` + `uv_run(UV_RUN_ONCE)` has more overhead per invocation.
- **Batch timers** — libuv scales dramatically better. At 1000 timers, libuv is 4.3× faster. Key differences:
  1. **Heap implementation**: libuv uses a min-heap with optimized sift operations. xKit's heap may have higher constant factors or less cache-friendly layout.
  2. **Batch processing**: libuv fires all expired timers in a tight loop within `uv__run_timers()`, while xKit acquires/releases `timer_mu` around each timer pop.
  3. **Timer allocation**: xKit `malloc`s each timer struct; libuv's `uv_timer_t` is pre-initialized and reused.

> **Optimization opportunity**: Remove the per-pop mutex lock/unlock in the timer dispatch loop (acquire once, pop all expired, release once). Consider a pre-allocated timer pool to eliminate malloc overhead.

### Offload Round-Trip (Submit → Done Callback)

#### xKit

| Benchmark | Time (ns) | CPU (ns) | Throughput |
| --- | ---: | ---: | ---: |
| `BM_EventLoop_OffloadSingle` | 6,959 | 4,110 | 243.3k items/s |
| `BM_EventLoop_OffloadBatch/10` | 18,514 | 15,058 | 664.1k items/s |
| `BM_EventLoop_OffloadBatch/100` | 82,536 | 66,319 | 1.51M items/s |
| `BM_EventLoop_OffloadBatch/1000` | 636,981 | 507,346 | 1.97M items/s |

#### libuv

| Benchmark | Time (ns) | CPU (ns) | Throughput |
| --- | ---: | ---: | ---: |
| `BM_Libuv_OffloadSingle` | 6,169 | 3,536 | 282.8k items/s |
| `BM_Libuv_OffloadBatch/10` | 13,879 | 10,394 | 962.1k items/s |
| `BM_Libuv_OffloadBatch/100` | 38,978 | 33,966 | 2.94M items/s |
| `BM_Libuv_OffloadBatch/1000` | 281,770 | 260,302 | 3.84M items/s |

#### Comparison (CPU time)

| Batch Size | xKit (CPU ns) | libuv (CPU ns) | Ratio |
| ---: | ---: | ---: | ---: |
| 1 | 4,110 | 3,536 | libuv 1.16× faster |
| 10 | 15,058 | 10,394 | libuv 1.45× faster |
| 100 | 66,319 | 33,966 | libuv 1.95× faster |
| 1,000 | 507,346 | 260,302 | libuv 1.95× faster |

**Analysis:**

- **Single offload** — Nearly tied (~1.16× gap). Both are dominated by the same bottleneck: waking a sleeping worker thread via kernel syscall.
- **Batch offload** — libuv is consistently ~2× faster at scale. The gap stabilizes at 1.95× for batch sizes ≥100. Key differences:
  1. **Completion notification**: libuv workers post to an async handle and the loop drains all completions in one `uv__work_done()` call. xKit workers push to an MPSC queue and each triggers a separate wake pipe write.
  2. **Allocation**: libuv's `uv_work_t` is caller-allocated (stack or embedded). xKit mallocs a `struct xEventWork_` per submit.
  3. **Wake coalescing**: libuv's async handle naturally coalesces multiple signals. xKit writes to the wake pipe per completion, though the pipe's EAGAIN handling provides some implicit coalescing.

## Summary

| Dimension | xKit vs libuv | Notes |
| --- | --- | --- |
| Wake Latency | libuv **2.1× faster** | Pipe drain overhead |
| Timer (single) | xKit **1.6× faster** | Simpler code path |
| Timer (batch) | libuv **4–5× faster** | Mutex per-pop + malloc overhead |
| Offload (single) | libuv **1.2× faster** | Essentially tied |
| Offload (batch) | libuv **~2× faster** | Batch drain + zero-alloc model |

## Opportunities for Improvement

1. **Timer dispatch without per-pop locking**: Acquire `timer_mu` once, pop all expired timers into a local list, release the lock, then fire them. This eliminates N lock/unlock cycles for N expired timers.

2. **Timer struct pooling**: Pre-allocate timer structs from a freelist (similar to the TLS freelist in xTask) to eliminate `malloc`/`free` per timer.

3. **Wake coalescing for offload**: Instead of writing to the wake pipe per completed work item, use an atomic flag + single wake. If the flag is already set, skip the pipe write. This matches libuv's `uv_async_send` semantics.

4. **Caller-allocated work items**: Allow `xEventLoopSubmitInline(loop, work_t*, ...)` where the caller provides the work struct, eliminating the per-submit malloc — matching libuv's `uv_work_t` model.

5. **Lighter wake mechanism on macOS**: Investigate using `__ulock_wait` / `__ulock_wake` (already used by xNote) instead of the pipe for the event loop wake path. This could halve the wake latency.
