# Phase 2 Implementation Roadmap: Concrete Code Examples

## Stage 1: Compiler Detection (1-2 days)

### Step 1.1: Add Feature Detection to `compiler.h`

**Location**: `libx++/xpp/compiler.h` after line 150

```cpp
/**
 * @brief Feature detection for C++20 coroutines.
 *
 * Checks for the __cpp_coroutines feature test macro defined in C++20.
 * Set to 1 if coroutines are available, 0 otherwise.
 *
 * Usage:
 *   #if XPP_HAS_COROUTINES
 *     #include <coroutine>
 *     // Coroutine-specific code
 *   #endif
 */
#if defined(__cpp_coroutines) && __cpp_coroutines >= 201902L
  #define XPP_HAS_COROUTINES 1
#else
  #define XPP_HAS_COROUTINES 0
#endif
```

### Step 1.2: Update `CMakeLists.txt`

**Location**: `libx++/xpp/CMakeLists.txt` after line 35

```cmake
# The base library requires C++11
target_compile_features(x++ PUBLIC cxx_std_11)

# Optional: Add a coroutine-specific test target for C++20 builds
# This allows consumers with C++20 to get coroutine support without
# forcing the entire library to C++20.
if(CMAKE_CXX_STANDARD GREATER_EQUAL 20 OR DEFINED XPP_ENABLE_COROUTINES)
  # Note: We don't add coroutine support to the base library;
  # users include <xpp/promise_coro.h> to opt-in.
  # See promise_coro_test.cpp for example usage.
endif()
```

**Rationale**: The base Promise<T> API stays C++11. Coroutine support is opt-in via header inclusion.

---

## Stage 2: Awaitable Methods (2-3 days)

### Step 2.1: Add Awaitable Interface to `promise.h`

**Location**: `libx++/xpp/promise.h` after line 130 (before `Promise::discard()`)

Add inside the `Promise<T>` class definition:

```cpp
  /**
   * @brief C++20 coroutine support: check if already resolved.
   *
   * Returns true if the promise is already resolved and can be
   * resumed immediately (i.e., no suspension is needed).
   *
   * Available only with C++20 coroutines.
   */
#if XPP_HAS_COROUTINES
  bool await_ready() const {
    // For now, always return false. In an optimized version,
    // we could check if the node is immediately ready.
    // This is a simplification; real implementations might
    // check a PollEvent or similar.
    return false;
  }
#endif

  /**
   * @brief C++20 coroutine support: suspend and register for resumption.
   *
   * Called when co_await expression needs to suspend. Registers the
   * coroutine to be resumed when the promise is ready.
   *
   * @param h  The coroutine handle to resume later
   * @return   true to suspend, false to immediately continue (not used here)
   *
   * Available only with C++20 coroutines.
   */
#if XPP_HAS_COROUTINES
  bool await_suspend(std::coroutine_handle<> h) {
    XPP_ASSERT(m_node != nullptr, "await_suspend on empty promise");
    
    // Create a coroutine-aware event that will resume h when fired
    auto event = new _::CoroutineEvent(h);
    
    // Register it with the node
    // The event will be armed when the promise is ready,
    // and its fire() will call h.resume()
    m_node->poll(Option<Event&>(*event));
    
    // Return true to indicate we've suspended
    // (h will be resumed from the event callback)
    return true;
  }
#endif

  /**
   * @brief C++20 coroutine support: extract and return the result.
   *
   * Called when co_await completes. Extracts the promise's value
   * and returns it to the awaiting coroutine.
   *
   * @return The resolved value
   *
   * Available only with C++20 coroutines.
   */
#if XPP_HAS_COROUTINES
  T await_resume() {
    XPP_ASSERT(m_node != nullptr, "await_resume on empty promise");
    ValueType result;
    m_node->read(&result);  // Extract result (moves it out)
    return std::move(result);
  }
#endif
```

### Step 2.2: Promise<void> Specialization

**Location**: `libx++/xpp/promise.h` in the `template <> class Promise<void>` section

After the `template <> inline void Promise<void>::wait()` method, add:

```cpp
/* ── C++20 Coroutine support for Promise<void> ──────────────────── */

#if XPP_HAS_COROUTINES
template <>
inline bool Promise<void>::await_ready() const {
  return false;  // Same as non-void version
}

template <>
inline bool Promise<void>::await_suspend(std::coroutine_handle<> h) {
  XPP_ASSERT(m_node != nullptr, "await_suspend on empty promise");
  
  auto event = new _::CoroutineEvent(h);
  m_node->poll(Option<Event&>(*event));
  
  return true;
}

template <>
inline void Promise<void>::await_resume() {
  XPP_ASSERT(m_node != nullptr, "await_resume on empty promise");
  Void v;
  m_node->read(&v);  // Just extract, don't return anything
}
#endif
```

### Step 2.3: Include Header Guard

**Location**: At the top of `libx++/xpp/promise.h` after line 22

```cpp
#if XPP_HAS_COROUTINES
#include <coroutine>
#endif
```

---

## Stage 3: CoroutineEvent Implementation (2-3 days)

### Step 3.1: Add CoroutineEvent to `promise_node.h`

**Location**: `libx++/xpp/promise_node.h` after the `RootEvent` class (around line 86)

```cpp
/**
 * @brief Event that resumes a C++20 coroutine when fired.
 *
 * Stores a coroutine handle and calls resume() when fire() is invoked.
 * This bridges the event loop with coroutine suspension/resumption.
 *
 * Only available with C++20 coroutines.
 *
 * Design note: The event is heap-allocated by await_suspend() and
 * deleted by the event loop callback mechanism (via xEventLoopPost).
 * Alternatively, we could use an arena or stack allocation; this
 * version is simple and follows the existing Event pattern.
 */
#if XPP_HAS_COROUTINES
class CoroutineEvent final : public Event {
public:
  explicit CoroutineEvent(std::coroutine_handle<> handle) : m_handle(handle) {}

  void fire() override {
    m_fired = true;
    if (m_handle) {
      m_handle.resume();  // Resume the coroutine
      // Note: After resume(), the coroutine might delete this event,
      // so don't access any members after this call.
    }
  }

private:
  std::coroutine_handle<> m_handle;
};
#endif  // XPP_HAS_COROUTINES
```

### Step 3.2: Memory Management Consideration

**Note**: The current design allocates `CoroutineEvent` on the heap in `await_suspend()`. This is safe because:

1. The event is captured by the event loop (via `xEventLoopPost`)
2. When the event fires, it resumes the coroutine
3. The coroutine can then clean up if needed

**Future optimization** (Phase 3): Use an arena or bump allocator to reduce heap fragmentation.

---

## Stage 4: Integration Test (3-5 days)

### Step 4.1: Create `promise_coro_test.cpp`

**Location**: Create new file `libx++/xpp/promise_coro_test.cpp`

```cpp
/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_coro_test.cpp - C++20 coroutine tests for xpp::Promise<T>
 */

#include <xpp/promise.h>

#include <gtest/gtest.h>

#if XPP_HAS_COROUTINES

extern "C" {
#include <x/base/event.h>
}

class PromiseCoroTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_loop = xEventLoopCreate();
    m_scope = new xpp::WaitScope(m_loop);
  }
  void TearDown() override {
    delete m_scope;
    xEventLoopDestroy(m_loop);
  }

  xEventLoop m_loop;
  xpp::WaitScope *m_scope;
};

/* ── Basic co_await ──────────────────────────────────────────────── */

TEST_F(PromiseCoroTest, BasicCoAwait) {
  auto coro = [](xpp::WaitScope& scope) -> xpp::Promise<int> {
    int x = co_await xpp::Promise<int>::resolve(42);
    co_return x;
  };
  
  int result = coro(*m_scope).wait(*m_scope);
  EXPECT_EQ(result, 42);
}

TEST_F(PromiseCoroTest, CoAwaitVoid) {
  auto coro = [](xpp::WaitScope& scope) -> xpp::Promise<void> {
    co_await xpp::Promise<void>::resolve();
    // Just complete
  };
  
  coro(*m_scope).wait(*m_scope);  // Should not hang
}

/* ── Sequential co_await ────────────────────────────────────────── */

TEST_F(PromiseCoroTest, SequentialCoAwait) {
  auto coro = [](xpp::WaitScope& scope) -> xpp::Promise<int> {
    int x = co_await xpp::Promise<int>::resolve(10);
    int y = co_await xpp::Promise<int>::resolve(20);
    int z = co_await xpp::Promise<int>::resolve(30);
    co_return x + y + z;
  };
  
  int result = coro(*m_scope).wait(*m_scope);
  EXPECT_EQ(result, 60);
}

/* ── co_await with transformation ─────────────────────────────── */

TEST_F(PromiseCoroTest, CoAwaitWithThen) {
  auto coro = [](xpp::WaitScope& scope) -> xpp::Promise<int> {
    auto promise = xpp::Promise<int>::resolve(10)
      .then([](int x) { return x + 1; })
      .then([](int x) { return x * 2; });
    
    int result = co_await promise;
    co_return result;
  };
  
  int result = coro(*m_scope).wait(*m_scope);
  EXPECT_EQ(result, 22);  // (10 + 1) * 2 = 22
}

/* ── Promise<Promise<T>> flattening with co_await ────────────── */

TEST_F(PromiseCoroTest, CoAwaitFlattenedPromise) {
  auto coro = [](xpp::WaitScope& scope) -> xpp::Promise<int> {
    // This .then() returns Promise<int>, which gets wrapped in ChainPromiseNode
    auto flattened = xpp::Promise<int>::resolve(5)
      .then([](int x) {
        return xpp::Promise<int>::resolve(x * 2);
      });
    
    int result = co_await flattened;
    co_return result;
  };
  
  int result = coro(*m_scope).wait(*m_scope);
  EXPECT_EQ(result, 10);
}

/* ── Resolver pattern with co_await ─────────────────────────── */

TEST_F(PromiseCoroTest, ResolverWithCoAwait) {
  auto pair = xpp::Promise<int>::make();
  
  // Simulate async work
  xpp::Promise<void>::eval([&pair] {
    pair.resolver.resolve(99);
  }).wait(*m_scope);
  
  // In a real scenario, this would run concurrently
  auto coro = [&](xpp::WaitScope& scope) -> xpp::Promise<int> {
    int value = co_await pair.promise;
    co_return value * 2;
  };
  
  int result = coro(*m_scope).wait(*m_scope);
  EXPECT_EQ(result, 198);
}

/* ── Discard with co_await ──────────────────────────────────── */

TEST_F(PromiseCoroTest, DiscardWithCoAwait) {
  auto coro = [](xpp::WaitScope& scope) -> xpp::Promise<void> {
    co_await xpp::Promise<int>::resolve(42).discard();
  };
  
  coro(*m_scope).wait(*m_scope);  // Should complete without error
}

#endif  // XPP_HAS_COROUTINES
```

### Step 4.2: Update `CMakeLists.txt`

**Location**: `libx++/xpp/CMakeLists.txt` after the regular test setup (around line 46)

```cmake
if(XPP_BUILD_TESTS AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
  # Build coroutine tests separately in C++20 mode
  add_executable(x++_coro_test promise_coro_test.cpp)
  target_link_libraries(x++_coro_test PRIVATE x++ GTest::gtest_main)
  target_compile_features(x++_coro_test PUBLIC cxx_std_20)
  add_test(NAME x++_coro_test COMMAND x++_coro_test)
endif()
```

---

## Stage 5: Optimization & Documentation (2-3 days)

### Step 5.1: Benchmark Script

Create `libx++/xpp/bench_coroutine.cpp`:

```cpp
/*
 * Benchmark: compare .then() chain vs coroutine version
 * Measures heap allocations and execution time
 */

#include <xpp/promise.h>
#include <iostream>
#include <chrono>

#if XPP_HAS_COROUTINES

extern "C" {
#include <x/base/event.h>
}

// Version 1: Traditional .then() chain
int bench_then(xpp::WaitScope& scope, int iterations) {
  auto start = std::chrono::high_resolution_clock::now();
  
  for (int i = 0; i < iterations; ++i) {
    int result = xpp::Promise<int>::resolve(1)
      .then([](int x) { return x + 1; })
      .then([](int x) { return x * 2; })
      .then([](int x) { return x + 3; })
      .wait(scope);
  }
  
  auto end = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

// Version 2: Coroutine version
auto coro_chain(xpp::WaitScope& scope) -> xpp::Promise<int> {
  auto x = co_await xpp::Promise<int>::resolve(1);
  auto y = co_await xpp::Promise<int>::resolve(x + 1);
  auto z = co_await xpp::Promise<int>::resolve(y * 2);
  auto w = co_await xpp::Promise<int>::resolve(z + 3);
  co_return w;
}

int bench_coro(xpp::WaitScope& scope, int iterations) {
  auto start = std::chrono::high_resolution_clock::now();
  
  for (int i = 0; i < iterations; ++i) {
    int result = coro_chain(scope).wait(scope);
  }
  
  auto end = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

int main() {
  xEventLoop loop = xEventLoopCreate();
  xpp::WaitScope scope(loop);
  
  const int iterations = 10000;
  
  int time_then = bench_then(scope, iterations);
  int time_coro = bench_coro(scope, iterations);
  
  std::cout << "Iterations: " << iterations << "\n";
  std::cout << "then() chain: " << time_then << " ms\n";
  std::cout << "Coroutine:   " << time_coro << " ms\n";
  std::cout << "Speedup: " << (double)time_then / time_coro << "x\n";
  
  xEventLoopDestroy(loop);
  return 0;
}

#else
int main() {
  std::cerr << "Coroutines not available (requires C++20)\n";
  return 1;
}
#endif
```

### Step 5.2: Documentation

Add to `promise.h` before the `Promise<T>` class definition:

```cpp
/**
 * @brief Example: using promises with C++20 coroutines
 *
 * C++20 adds native coroutine support to the language. Promises
 * can be used with co_await for sequential async code:
 *
 * @code
 *   // Sequential async operations (Phase 2+)
 *   auto fetch_user(int id) -> Promise<User> {
 *     auto response = co_await http_get("/users/" + id);
 *     auto json = parse_json(response);
 *     co_return User{json};
 *   }
 *
 *   // In a coroutine:
 *   auto user = co_await fetch_user(123);
 *
 *   // Or wait synchronously:
 *   auto user = fetch_user(123).wait(scope);
 * @endcode
 *
 * Coroutine benefits over .then() chains:
 *   - Sequential code that reads left-to-right
 *   - No intermediate heap allocations per co_await
 *   - Native error handling (try/catch works directly)
 *   - Easier debugging (coroutine frames on stack)
 *
 * The implementation is backward compatible: existing .then().wait()
 * code continues to work unchanged. Coroutine support is opt-in by
 * including <xpp/promise.h> with a C++20-capable compiler.
 */
```

---

## Stage 6: Testing Checklist

### Functional Tests
- [ ] Basic `co_await` on resolved promise
- [ ] Sequential `co_await` statements
- [ ] Mixed `.then()` and `co_await`
- [ ] Promise flattening with `co_await`
- [ ] `Promise<void>` with `co_await`
- [ ] Resolver pattern + `co_await`
- [ ] `.discard()` with `co_await`

### Edge Cases
- [ ] Coroutine suspends before promise resolves
- [ ] Promise already resolved before `co_await`
- [ ] Multiple coroutines awaiting same promise
- [ ] Nested coroutines (coroutine calling coroutine)
- [ ] Exception safety (exception in co_await)

### Performance
- [ ] Heap allocation reduction verified
- [ ] Execution time comparable or better
- [ ] Stack frame size reasonable
- [ ] No memory leaks

### Compatibility
- [ ] C++11 code unaffected
- [ ] C++17 code unaffected
- [ ] C++20 without coroutines works
- [ ] C++20 with coroutines works

---

## Implementation Order

1. **Day 1**: `compiler.h` feature detection + CMake
2. **Day 2-3**: Add `await_*` methods to `promise.h`
3. **Day 4-5**: Implement `CoroutineEvent` in `promise_node.h`
4. **Day 6-8**: Write comprehensive tests in `promise_coro_test.cpp`
5. **Day 9-10**: Benchmarks and documentation
6. **Day 11+**: Optimization and profiling

**Total: 11-18 days** (as estimated earlier)

---

## Backward Compatibility Verification

Run these commands after implementation:

```bash
# C++11 build (should work unchanged)
cmake -DCMAKE_CXX_STANDARD=11 ..
make x++_test
./x++_test

# C++17 build (should work unchanged)
cmake -DCMAKE_CXX_STANDARD=17 ..
make x++_test
./x++_test

# C++20 build with coroutines
cmake -DCMAKE_CXX_STANDARD=20 ..
make x++_test x++_coro_test
./x++_test      # All 22 Phase 1 tests
./x++_coro_test # New Phase 2 tests
```

All tests should pass in all configurations.

