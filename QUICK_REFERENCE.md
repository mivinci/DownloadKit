# Phase 2 Coroutine Integration - Quick Reference

## TL;DR

**Status**: libx++ Promise<T> is **architecturally ready** for C++20 coroutines. The poll/event design was built for this.

**What you get in Phase 2**:
```cpp
// Current (Phase 1):
int result = promise
  .then([](int x) { return x + 1; })
  .then([](int x) { return x * 2; })
  .wait(scope);  // Blocks thread

// Phase 2 (with coroutines):
int result = co_await promise
  .then([](int x) { return x + 1; })
  .then([](int x) { return x * 2; });  // No wait(), no heap per .then()
```

---

## Current Implementation: One-Liner Summaries

| Component | What It Does | Key Insight |
|-----------|-------------|-------------|
| **Promise<T>** | Owns a PromiseNode; provides .then(), .wait() API | Move-only, type generic via node hierarchy |
| **PromiseNode** | Virtual base: poll(event) + read(dest) | Two-phase: register event, then extract result |
| **ImmediatePromiseNode** | Already-resolved value | Fires event immediately |
| **TransformPromiseNode** | Applies func to dependency | Each .then() creates one; this is the heap bottleneck |
| **ChainPromiseNode** | Flattens Promise<Promise<T>> | Clever state machine: outer→inner adoption |
| **AdapterPromiseNode** | External resolution via Resolver<T> | Bridge between producer (Resolver) and consumer (Promise) |
| **YieldPromiseNode** | Defers to next event loop turn | Breadth-first execution |
| **Event** | Callback container for event loop | arm() posts to xEventLoop; fire() called from callback |
| **WaitScope** | Thread-local event loop registration | One per thread, stores thread-local xEventLoop |
| **.wait()** | Block until ready, extract result | Loop: poll(root), xEventWait, read when fired |

---

## Why Coroutines Win

### Problem (Phase 1): Heap Per Chain
```cpp
p.then(f).then(g).then(h).wait(scope)
// Allocates: ImmediatePromiseNode + TransformPromiseNode(f) 
//          + TransformPromiseNode(g) + TransformPromiseNode(h) + RootEvent
// Total: 4 heap allocations, linked list traversal on read
```

### Solution (Phase 2): Coroutine Frame
```cpp
auto coro() -> Promise<Result> {
  auto a = co_await p;
  auto b = co_await Promise::resolve(f(a));
  auto c = co_await Promise::resolve(g(b));
  auto d = co_await Promise::resolve(h(c));
  co_return d;  // Or return wrapped in Promise
}
// Single coroutine frame on stack, no transformation chain
// Local variables (a, b, c, d) stored in frame, not intermediate nodes
```

---

## The Key Files (File-by-File Review)

### 1. `promise.h` (384 lines)
- **Promise<T> class** (lines 44-195)
  - Factories: `resolve()`, `eval()`, `make()`
  - API: `.then(Func)`, `.wait(WaitScope&)`, `.discard()`
  - [TODO] Phase 2 will add: `await_ready()`, `await_suspend()`, `await_resume()`
  - Private: `Own<_::PromiseNode> m_node`
  
- **Resolver<T> class** (lines 197-244)
  - Non-owning pointer to AdapterPromiseNode
  - `.resolve(value)` method + `is_pending()`
  
- **WaitScope class** (lines 289-321)
  - Thread-local event loop registration
  - Constructor: stores in `tl_current_loop`
  - Destructor: nulls out `tl_current_loop`
  - Static: `current_loop()` getter
  
- **Key Methods** (lines 342-369)
  - `.wait(WaitScope&)`: poll(root) → xEventWait loop → read result
  - `.discard()`: return `.then([](T){})` to make Promise<void>

### 2. `promise_node.h` (420 lines)
- **Base class: PromiseNode** (lines 119-154)
  - `poll(Option<Event&>)`: register event or fire immediately
  - `read(void* dest)`: extract result via type erasure
  
- **5 concrete node types**:
  1. **ImmediatePromiseNode<T>** (lines 156-176): pre-resolved, fire immediately
  2. **TransformPromiseNode<U,T,Func>** (lines 178-277): apply func + 4 specializations
  3. **ChainPromiseNode** (lines 280-319): flatten Promise<Promise<T>>
  4. **AdapterPromiseNode<T>** (lines 321-388): externally resolved
  5. **YieldPromiseNode** (lines 390-404): defer to next turn
  
- **Event hierarchy** (lines 50-116)
  - **Event**: base class, `arm()` posts to loop, `fire()` sets flag
  - **RootEvent**: used by `.wait()`
  - **PollEvent**: deferred arm mechanism (stores event until ready)
  
- **Helper**: `maybe_chain()` wraps `Promise<Promise<T>>` in ChainPromiseNode

### 3. `promise.cpp` (85 lines)
- **WaitScope thread-local** (line 22): `static thread_local xEventLoop tl_current_loop`
- **WaitScope constructor** (lines 24-29): register in thread-local, assert one-per-thread
- **WaitScope destructor** (lines 31-32): null out thread-local
- **Event::arm()** (lines 48-57): post to xEventLoop or fire synchronously
- **ChainPromiseNode::fire()** (lines 61-81): state machine: extract inner node, adopt, re-poll

### 4. `compiler.h` (152 lines)
- **Compiler detection macros**: `XPP_UNLIKELY`, `XPP_LIKELY`, `XPP_NORETURN`, etc.
- **[TODO] Phase 2**: Add `XPP_HAS_COROUTINES` feature detection
  ```cpp
  #if defined(__cpp_coroutines) && __cpp_coroutines >= 201902L
    #define XPP_HAS_COROUTINES 1
  #else
    #define XPP_HAS_COROUTINES 0
  #endif
  ```

### 5. `CMakeLists.txt` (66 lines)
- **Line 35**: `target_compile_features(x++ PUBLIC cxx_std_11)`
- **[TODO] Phase 2**: Make C++20 optional for coroutine tests
  ```cmake
  # Add optional coroutine target
  if(CMAKE_CXX_STANDARD GREATER_EQUAL 20)
    add_executable(x++_coro_test promise_coro_test.cpp)
    target_link_libraries(x++_coro_test PRIVATE x++ GTest::gtest_main)
    add_test(NAME x++_coro_test COMMAND x++_coro_test)
  endif()
  ```

### 6. `promise_test.cpp` (204 lines)
- **22 test cases** covering:
  - Basic: resolve, eval, then, wait
  - Chaining: multiple .then() calls
  - Flattening: Promise<Promise<T>>
  - Resolver: producer/consumer pattern
  - Move semantics
- **[TODO] Phase 2**: Create `promise_coro_test.cpp`
  ```cpp
  // Basic co_await
  TEST_F(PromiseCoroTest, CoAwaitResolve) {
    auto coro = [](WaitScope& scope) -> Promise<int> {
      int x = co_await Promise<int>::resolve(42);
      co_return x * 2;
    };
    EXPECT_EQ(coro(scope).wait(scope), 84);
  }
  ```

---

## Architecture: The Poll/Event Model

```
Promise<T>  ← owns PromiseNode
    ↓
PromiseNode (virtual)
    ├─ poll(Option<Event&>)     ← "Register to be notified when ready"
    └─ read(void* dest)         ← "Extract result"

Event (virtual)
    ├─ arm()   ← "Post me to event loop"
    └─ fire()  ← "Called when ready"
    
WaitScope (thread-local)
    └─ xEventLoop ← thread-local storage
```

**Why this matters for coroutines**:
- `poll()` maps to `await_suspend()`: register coroutine to resume when ready
- `fire()` maps to `std::coroutine_handle<>::resume()`: resume the coroutine
- `read()` maps to `await_resume()`: extract result after resumption

---

## Phase 2 Checklist

### 1. Compiler Detection (1-2 days)
- [ ] Add `XPP_HAS_COROUTINES` to `compiler.h`
- [ ] Add CMake check for C++20 feature
- [ ] Test with `__cpp_coroutines >= 201902L`

### 2. Coroutine Integration (2-3 days)
- [ ] Add `std::coroutine_traits` specialization for `Promise<T>`
- [ ] Implement `await_ready()`: check if promise already resolved
- [ ] Implement `await_suspend(handle)`: store handle, register event
- [ ] Implement `await_resume()`: extract and return result
- [ ] Handle `Promise<void>` specialization

### 3. Event Loop Integration (2-3 days)
- [ ] Create `CoroutineEvent` class: stores `std::coroutine_handle<>`
- [ ] Override `fire()` to call `handle.resume()`
- [ ] Ensure event loop posts resume callback via `xEventLoopPost`

### 4. Tests (3-5 days)
- [ ] Basic `co_await promise`
- [ ] Chain: `co_await p1; co_await p2;`
- [ ] Mix sync/async: `.then()` after `co_await`
- [ ] Flattening: `co_await` of `Promise<Promise<T>>`
- [ ] Resolver pattern with coroutines

### 5. Optimization & Docs (2-3 days)
- [ ] Benchmark: verify heap allocations eliminated
- [ ] Profile: compare coroutine vs `.then()` codegen
- [ ] Document API surface with examples

**Total**: 11-18 days

---

## Integration Points

### Modify
1. **compiler.h**: Add feature detection
2. **CMakeLists.txt**: C++20 optional flag
3. **promise.h**: Add awaitable methods (conditional)
4. **promise_node.h**: Add CoroutineEvent class (conditional)
5. **promise.cpp**: Add coroutine resumption logic (conditional)

### Create
1. **promise_coro.h** (optional): Separate coroutine definitions
2. **promise_coro_test.cpp**: Comprehensive coroutine tests

### Backward Compatibility
- ✅ C++11 code: unaffected (feature macro guards everything)
- ✅ C++20 non-coroutine code: unaffected (opt-in)
- ✅ Existing `.then().wait()` API: unchanged

---

## Real-World Example: HTTP Client

### Phase 1 (Current)
```cpp
xpp::Promise<std::string> fetch(const std::string& url) {
  return xpp::Promise<void>::make()
    .then([url](auto p) {
      http_request_async(url, [p](auto response) {
        p.resolver.resolve(response);
      });
      return p;
    })
    .wait(scope);
}
```
**Issues**: Chain of .then(), callback nesting, unclear control flow

### Phase 2 (Coroutines)
```cpp
xpp::Promise<std::string> fetch(const std::string& url) {
  auto pair = xpp::Promise<std::string>::make();
  
  // Spawn async fetch
  http_request_async(url, [pair](auto response) {
    pair.resolver.resolve(response);
  });
  
  // Wait for it
  std::string result = co_await pair.promise;
  co_return result;
}

// Or more idiomatically:
xpp::Promise<std::string> fetch(const std::string& url) {
  // Assumes http library provides Promise-returning API
  auto data = co_await http::get_async(url);
  co_return parse(data);
}
```
**Benefits**: Sequential code, no nesting, clear exception handling (co_await + try/catch)

---

## Key Insight: ChainPromiseNode is Brilliant

The trick: when `.then()` returns `Promise<T>`, we need to flatten `Promise<Promise<T>>` to `Promise<T>`.

**How**: Promise<T> is literally `struct { Own<PromiseNode> node; }`.

So `ChainPromiseNode::fire()` reads the result as a "PromiseShell" struct, steals its node, and then polls that node instead:

```cpp
void ChainPromiseNode::fire() {
  struct PromiseShell { Own<PromiseNode> node; };
  PromiseShell shell;
  m_inner->read(&shell);        // Extract from outer promise
  m_inner = std::move(shell.node);  // Adopt inner node
  m_state = Step2;
  m_inner->poll(m_outer_event);     // Now poll the inner node
}
```

**Why Phase 2 coroutines don't break this**: Coroutines `.co_await promise` will also trigger flattening naturally because `maybe_chain()` still wraps nested promises in `ChainPromiseNode`.

---

## Design Philosophy

This design reflects **Rust's ownership model** adapted to C++:
- **Move-only types**: prevent use-after-free
- **Type erasure via virtual nodes**: generic event handling
- **Poll/event model**: composable, non-blocking
- **Void special case**: Rust-like unit type for no-value promises

The event loop integration is **minimal**: just `xEventLoopPost()` and `xEventWait()` calls. No magic, no runtime reflection.

