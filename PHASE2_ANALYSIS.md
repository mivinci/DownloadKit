# Phase 2: C++20 Coroutine Integration for libx++ Promise<T>
## Comprehensive Implementation Guide

---

## Executive Summary

The libx++ Promise<T> system is **architecturally well-positioned for C++20 coroutine integration**. The current implementation uses a poll-based event model (`poll(Option<Event&>)`) that was explicitly designed with coroutines in mind (as noted in the promise.h comments). Phase 2 will add `co_await` support while maintaining backward compatibility with the existing `.then().wait()` API.

**Current Status**: C++11-only, with explicit TODO comments about Phase 2 coroutines in `promise.h:89-90`.

---

## Part 1: Current Promise<T> Implementation Analysis

### 1.1 Promise<T> Class (`promise.h`, lines 44-195)

#### Core Structure
```cpp
template <class T> class Promise {
  Own<_::PromiseNode> m_node;  // Single ownership pointer
```

**Key Design Patterns:**
- **Move-only semantics**: Non-copyable, move-constructible, encourages ownership transfer
- **Type erasure via nodes**: Uses polymorphic `PromiseNode` for type-generic event handling
- **Template specializations**: `PromiseForResult<>`, `PromiseForResultVoid<>` handle `.then()` return types

#### Static Factories

| Factory | Purpose | Example |
|---------|---------|---------|
| `Promise::resolve(T)` | Immediate resolution | `Promise<int>::resolve(42)` → Wrapped in `ImmediatePromiseNode` |
| `Promise::resolve()` | Promise<void> variant | `Promise<void>::resolve()` → No value stored |
| `Promise::eval(Func)` | Deferred execution | `Promise<void>::eval([] { return 42; })` → `YieldPromiseNode` + `TransformPromiseNode` |
| `Promise::make()` | Create promise+resolver pair | `auto [p, r] = Promise<int>::make(); r.resolve(42);` |

**Critical Detail**: `make()` returns `PromiseAndResolver<T>` struct (line 276):
```cpp
struct PromiseAndResolver {
  Promise<T>  promise;
  Resolver<T> resolver;
};
```

#### Chaining API: `.then(Func)`

**Two overloads** (lines 81-112) via SFINAE:

1. **Non-void promise** (lines 81-95):
   ```cpp
   template <class Func, class V = ValueType,
             class = typename std::enable_if<!std::is_same<V, Void>::value>::type>
   auto then(Func &&func)
     -> Promise<typename ReducePromise<decltype(std::declval<Func>()(std::declval<V>()))>::Type>
   ```
   - Accepts function taking `V` (the promise's value type)
   - Return type deduced and flattened via `ReducePromise<>`

2. **Void promise** (lines 98-112):
   ```cpp
   template <class Func, class V = ValueType,
             class = typename std::enable_if<std::is_same<V, Void>::value>::type,
             class = void>  // Extra disambiguator
   auto then(Func &&func)
     -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type>
   ```
   - Accepts no-argument function

**Heap Allocation Note** (TODO, line 89-90):
```cpp
// TODO: each then() heap-allocates a node. Phase 2 (coroutines)
// eliminates most chains; remaining hot paths can use an arena.
```
This is precisely where coroutines win: eliminates the chain of heap allocations by using the coroutine frame instead.

#### Synchronous Wait: `.wait(WaitScope &scope)`

**Implementation** (lines 344-369):
```cpp
template <class T> T Promise<T>::wait(WaitScope &scope) {
  XPP_ASSERT(m_node != nullptr, "Promise::wait on empty promise");
  _::RootEvent root;
  m_node->poll(root);  // Register root event with node

  while (!root.fired()) {
    xEventWait(scope.loop(), -1);  // Block until event fires
  }

  ValueType result;
  m_node->read(&result);  // Extract and return result
  return std::move(result);
}
```

**Event Loop Integration**:
1. Create a `RootEvent` (simple flag-setter)
2. Register it with the node via `poll()`
3. Block on `xEventWait()` until event is armed
4. Extract result via `read()`

The `-1` timeout means "block forever until an event arrives."

---

### 1.2 PromiseNode Hierarchy (`promise_node.h`)

#### Base Class: PromiseNode (lines 119-154)

```cpp
class PromiseNode {
  virtual void poll(Option<Event &> event) = 0;
  virtual void read(void *dest) = 0;
};
```

**Two-phase protocol**:
1. **`poll(Option<Event &> event)`**: "Register this event to be armed when I'm ready. If already ready, arm immediately."
   - `event` is pass-by-reference to avoid allocation
   - `Option<Event&>` is `none` if caller doesn't want to wait (used internally)

2. **`read(void *dest)`**: "Give me your result."
   - Type erasure via void pointer
   - **Semantics**: Move the value out; calling twice is undefined
   - Only valid after `poll()` has fired the event

#### Concrete Node Types

##### 1. ImmediatePromiseNode<T> (lines 156-176)
```cpp
template <class T>
class ImmediatePromiseNode final : public PromiseNode {
  T m_value;
  
  void poll(Option<Event &> event) override {
    if (event.is_some()) event.unwrap().arm();  // Fire immediately
  }
  
  void read(void *dest) override {
    *static_cast<T *>(dest) = std::move(m_value);
  }
};
```

**Use**: `Promise::resolve(value)`

##### 2. TransformPromiseNode<U, T, Func> (lines 178-209, with 4 specializations)
```cpp
template <class U, class T, class Func>
class TransformPromiseNode final : public PromiseNode {
  Own<PromiseNode> m_dep;
  Func m_func;
  
  void poll(Option<Event &> event) override {
    m_dep->poll(event);  // Delegate to dependency
  }
  
  void read(void *dest) override {
    T dep_value;
    m_dep->read(&dep_value);           // Pull from dependency
    *static_cast<U *>(dest) = m_func(std::move(dep_value));  // Apply func
  }
};
```

**Specializations**:
- `T=Void, U!=Void`: dependency is `Promise<void>`, func takes no args
- `U=Void, T!=Void`: func returns void, dependency produces value
- `U=Void, T=Void`: both sides void

**Use**: Created by `.then(func)`. Forms the chain nodes.

**Heap Allocation**: Each `.then()` creates a new `TransformPromiseNode` on the heap. This is the **key bottleneck** that Phase 2 (coroutines) eliminates.

##### 3. ChainPromiseNode (lines 280-319)
```cpp
class ChainPromiseNode final : public PromiseNode, public Event {
  enum State { Step1, Step2 };
  State m_state;
  Own<PromiseNode> m_inner;
  Option<Event &> m_outer_event;
  
  ChainPromiseNode(Own<PromiseNode> inner) : m_state(Step1), m_inner(std::move(inner)) {
    m_inner->poll(Option<Event &>(*this));  // Self-register as event
  }
  
  void poll(Option<Event &> event) override;
  void fire() override;  // Defined in promise.cpp
};
```

**Purpose**: Flattens `Promise<Promise<T>>` → `Promise<T>`. 

**State Machine**:
1. **Step1**: Outer promise not yet ready. When outer fires:
   - Call `fire()` → extract inner Promise's node
   - Adopt inner node, transition to Step2
   - Re-poll the inner node with the outer event
2. **Step2**: Inner promise ready. Delegate all `poll()`/`read()` to inner node.

**Implementation detail** (promise.cpp, lines 61-81):
```cpp
void ChainPromiseNode::fire() {
  // All Promise<T> are: struct { Own<PromiseNode> node; }
  // We read the dependency into a shell struct and steal its node
  struct PromiseShell {
    Own<PromiseNode> node;
  };
  PromiseShell shell;
  m_inner->read(&shell);        // Extract Promise's node
  
  m_inner = std::move(shell.node);  // Adopt it
  m_state = Step2;
  m_inner->poll(m_outer_event);     // Re-poll with outer event
  m_fired = true;
}
```

**Clever design**: Reads the Promise as a shell struct to extract its node. This works because `Promise<T>` has a single `Own<PromiseNode>` member.

##### 4. AdapterPromiseNode<T> (lines 321-388)
```cpp
template <class T>
class AdapterPromiseNode final : public PromiseNode {
  T m_value;
  PollEvent m_poll;
  bool m_resolved;
  
  void poll(Option<Event &> event) override {
    if (m_resolved) {
      if (event.is_some()) event.unwrap().arm();
    } else {
      m_poll.init(event);  // Register for later
    }
  }
  
  void read(void *dest) override {
    XPP_ASSERT(m_resolved, "read before resolve");
    *static_cast<T *>(dest) = std::move(m_value);
  }
  
  void resolve(T &&value) {
    XPP_ASSERT(!m_resolved, "resolved twice");
    m_value = std::move(value);
    m_resolved = true;
    m_poll.arm();  // Fire any waiting event
  }
};
```

**Use**: `Promise::make()` → `Resolver<T>::resolve()`

##### 5. YieldPromiseNode (lines 390-404)
```cpp
class YieldPromiseNode final : public PromiseNode {
  void poll(Option<Event &> event) override {
    if (event.is_some()) event.unwrap().arm();
  }
  
  void read(void *dest) override {
    *static_cast<Void *>(dest) = Void{};
  }
};
```

**Use**: `Promise::eval(func)`. Defers func to the next event loop turn.

#### Event Classes (lines 50-116)

##### Event (lines 59-77)
```cpp
class Event {
  bool m_fired;
  
  void arm();  // Posts callback to xEventLoop
  bool fired() const { return m_fired; }
  virtual void fire() { m_fired = true; }
};
```

**Implementation** (promise.cpp, lines 48-57):
```cpp
void Event::arm() {
  if (m_fired) return;
  xEventLoop loop = tl_current_loop;
  if (loop) {
    xEventLoopPost(loop, event_post_callback, this);
  } else {
    fire();  // No loop — fire synchronously
  }
}

static void event_post_callback(void *arg) {
  auto *event = static_cast<Event *>(arg);
  event->fire();
}
```

**Bridge to C event loop**: Posts a callback to `xEventLoop` (from libx C API).

##### RootEvent (lines 82-85)
Simple subclass that sets `m_fired = true` when `fire()` is called. Used by `wait()`.

##### PollEvent (lines 91-116)
Tracks whether the promise is already ready (for deferred arm):
```cpp
class PollEvent {
  Option<Event &> m_event;
  bool m_ready;
  
  void init(Option<Event &> event) {
    if (m_ready) {
      if (event.is_some()) event.unwrap().arm();  // Already ready
    } else {
      m_event = event;  // Store for later
    }
  }
  
  void arm() {
    m_ready = true;
    if (m_event.is_some()) {
      m_event.unwrap().arm();  // Fire the stored event
      m_event = none;
    }
  }
};
```

---

### 1.3 Resolver<T> & PromiseAndResolver (lines 197-287)

#### Resolver<T>
```cpp
template <class T> class Resolver {
  _::AdapterPromiseNode<ValueType> *m_node;  // Non-owning
  
  void resolve(ValueType &&value) {
    XPP_ASSERT(m_node != nullptr, "already consumed or moved-from");
    m_node->resolve(std::move(value));
    m_node = nullptr;  // Null out to prevent double-resolve
  }
  
  bool is_pending() const { return m_node != nullptr; }
};
```

**Specialization for `void`** (lines 247-272): Same pattern, `resolve()` takes no args.

#### PromiseAndResolver<T>
```cpp
template <class T> struct PromiseAndResolver {
  Promise<T>  promise;
  Resolver<T> resolver;
};
```

Returned by `Promise<T>::make()`. Pairs the producer and consumer.

---

### 1.4 WaitScope & Event Loop Integration (promise.h lines 289-321, promise.cpp lines 20-57)

#### WaitScope
```cpp
class WaitScope {
  xEventLoop m_loop;
  
  WaitScope(xEventLoop loop);   // Register in thread-local
  ~WaitScope();                 // Deregister
  
  static xEventLoop current_loop();  // Get thread-local
};
```

**Thread-local storage** (promise.cpp, line 22):
```cpp
static thread_local xEventLoop tl_current_loop = nullptr;
```

**One-per-thread invariant** (promise.cpp, lines 26-27):
```cpp
XPP_ASSERT(tl_current_loop == nullptr,
           "WaitScope: this thread already has an active WaitScope");
```

Ensures exactly one event loop per thread.

---

## Part 2: C++ Standard Version & Compiler Support

### 2.1 Current Compilation Target: C++11

**CMakeLists.txt (line 35)**:
```cmake
target_compile_features(x++ PUBLIC cxx_std_11)
```

**Implication**: All Promise code must currently be C++11-compatible. The TODO comment about Phase 2 implies that coroutines (C++20) should become **optional**, not mandatory.

### 2.2 No Existing C++20 Usage

- ✅ **No `__cpp_impl_coroutine` checks** anywhere
- ✅ **No `co_await`, `co_return`, `co_yield`** in the codebase
- ✅ **No coroutine includes** (`<coroutine>`)
- ✅ **No conditional compilation** for C++20

**Conclusion**: Phase 2 must use **opt-in conditional compilation** via feature macros.

---

## Part 3: Test Coverage & API Surface

### 3.1 Existing Test Suite (promise_test.cpp)

**Setup** (lines 17-30):
```cpp
class PromiseTest : public ::testing::Test {
  xEventLoop m_loop;
  xpp::WaitScope *m_scope;
};
```

**Test Categories**:

1. **Basic resolution** (lines 34-42):
   - `ResolveInt`: immediate integer resolution
   - `ResolveVoid`: immediate void completion

2. **Deferred execution** (lines 46-57):
   - `EvalLaterInt`: `eval()` returns int later
   - `EvalLaterVoid`: `eval()` defers side-effect

3. **Chaining** (lines 61-100):
   - Single transforms: `T→U`, `T→void`, `void→U`, `void→void`
   - Chained transforms: multiple `.then()` calls
   - Return type deduction verified

4. **Promise flattening** (lines 131-164):
   - `.then()` returns `Promise<T>` → auto-flattened
   - Multi-level flattening: `Promise<Promise<Promise<T>>>`

5. **Resolver pattern** (lines 113-123, 196-203):
   - Create promise+resolver pair
   - Resolve asynchronously
   - Chain with `.then()` after creation

6. **Utility** (lines 125-129):
   - `yield()`: defer to next turn

7. **Move semantics** (lines 179-193):
   - Move construction and assignment
   - Moved-from state (operator bool returns false)

**Total**: 22 test cases covering the main API surface.

---

## Part 4: Architecture Decisions & Integration Points

### 4.1 Why Coroutines Fit Naturally

**The poll/event design is coroutine-ready**:

1. **No heap-allocated chain nodes**: In `.then()` chains, each callback currently requires a `TransformPromiseNode` heap allocation. Coroutines use the frame directly.

2. **Suspension points**: The `poll()` → event → `read()` protocol maps naturally to:
   - `poll()` = "I'm suspended, register to be resumed when ready"
   - Event fire = "I'm ready, resume the coroutine"
   - `read()` = Resume execution in the coroutine

3. **Type erasure via void pointer**: The `read(void *dest)` design allows type-safe extraction in the coroutine without knowing T statically.

4. **Non-blocking loop integration**: The event loop callback system (`xEventLoopPost`) provides the resumption mechanism.

### 4.2 Coroutine Interface Design (Proposed for Phase 2)

**Goal**: Make `Promise<T>` awaitable.

```cpp
// In <xpp/promise_coro.h>
template <class T> class Promise<T> {
  // ... existing API ...
  
  // NEW: C++20 coroutine support
  
  bool await_ready() const {
    // Poll the node without registering an event
    // Return true if already ready
  }
  
  auto await_suspend(std::coroutine_handle<> h) {
    // Register the coroutine to resume when ready
    // Return true/void depending on suspension strategy
  }
  
  T await_resume() {
    // Extract and return the result
    return std::move(result);
  }
};
```

**This design**:
- ✅ Maintains backward compatibility (`.then().wait()` still works)
- ✅ Uses existing `poll()`/`read()` infrastructure
- ✅ Eliminates heap allocations for chained coroutines
- ✅ Integrates with thread-local `WaitScope::current_loop()`

### 4.3 Compiler Detection Strategy

**Current approach**: Conditional via `__cpp_coroutines` (C++20 feature test macro).

**Placement** (proposed): Add to `compiler.h`

```cpp
// In xpp/compiler.h

// Feature detection for C++20 coroutines
#if defined(__cpp_coroutines) && __cpp_coroutines >= 201902L
  #define XPP_HAS_COROUTINES 1
#else
  #define XPP_HAS_COROUTINES 0
#endif
```

Then in `promise.h`:
```cpp
#if XPP_HAS_COROUTINES
  #include <coroutine>
  // Define await_ready, await_suspend, await_resume
#endif
```

---

## Part 5: Key Integration Points & Challenges

### 5.1 Integration with WaitScope & Event Loop

**Current**: `wait()` blocks on event loop → event fires → `read()` result → return.

**With coroutines**: The coroutine suspension/resumption replaces the blocking loop.

**Challenge**: A coroutine can't use the thread-local event loop the same way:
- `.wait()` requires a `WaitScope` in scope (single-threaded model)
- Coroutines are typically used at the top level, returning control

**Solution (Proposed)**:
1. Keep `.wait(WaitScope&)` for existing code
2. For coroutines, the event loop integration happens implicitly:
   - `co_await promise` suspends the coroutine
   - Event fires → resumption callback posted to event loop
   - User code drives the loop (`xEventWait()` or equivalent)

### 5.2 Void Type Handling in Coroutines

**Current**: `Promise<void>` stores `Void{}` (unit type).

**With await_resume()**: Must return `void` or `Void` depending on T.

**Solution**: Specialization:
```cpp
// Promise<void> specialization
template <>
inline void Promise<void>::await_resume() {
  Void v;
  m_node->read(&v);
  // Return nothing (void)
}
```

### 5.3 Type Erasure & await_resume()

**Challenge**: `read(void *dest)` uses type erasure. In `await_resume()`, we need to know T to allocate storage and cast correctly.

**Solution**: No problem! We're in a template specialization:
```cpp
template <class T>
T Promise<T>::await_resume() {
  T result;
  m_node->read(&result);  // type-safe cast
  return std::move(result);
}
```

### 5.4 State Management & ChainPromiseNode

**Current**: `ChainPromiseNode` is a state machine with `Step1`/`Step2`.

**Impact**: Coroutine flattening should "just work" because:
- `.then()` still creates `TransformPromiseNode`
- `maybe_chain()` wraps nested promises in `ChainPromiseNode`
- Coroutine awaits the final `Promise<T>` (after flattening)

---

## Part 6: Incremental Migration Path

### Phase 2 Milestones

**1. Compiler detection** (1-2 days)
   - Add `XPP_HAS_COROUTINES` to `compiler.h`
   - Add cmake feature check to `CMakeLists.txt`

**2. coroutine_traits specialization** (1-2 days)
   - Add `std::coroutine_traits<Promise<T>>` 
   - Map promise promise to coroutine protocol

**3. Awaitable methods** (2-3 days)
   - `await_ready()`: check if already resolved
   - `await_suspend(handle)`: register resumption
   - `await_resume()`: extract result

**4. Integration with Event::arm()** (2-3 days)
   - Create a coroutine-aware Event subclass
   - Store `std::coroutine_handle<>` instead of callback
   - Call `.resume()` when event fires

**5. Tests** (3-5 days)
   - Basic `co_await promise` tests
   - Chained `.then().wait()` after `co_await`
   - Mixed sync/async code paths

**6. Benchmarks & optimization** (2-3 days)
   - Verify heap allocations eliminated
   - Compare coroutine vs `.then()` codegen

**Estimated total**: 11-18 days for full Phase 2

### Backward Compatibility

- ✅ C++11 consumer code: unaffected (no coroutines included)
- ✅ C++20 without coroutines: unaffected (feature check)
- ✅ `.then().wait()` API: unchanged
- ✅ Promise factories: unchanged

---

## Part 7: Key Files & Locations for Phase 2

### To Modify

| File | Purpose | Change |
|------|---------|--------|
| `libx++/xpp/compiler.h` | Feature detection | Add `XPP_HAS_COROUTINES` |
| `libx++/xpp/CMakeLists.txt` | Build config | Add C++20 conditional |
| `libx++/xpp/promise.h` | Public API | Add `await_ready/suspend/resume` (conditional) |
| `libx++/xpp/promise_node.h` | Node hierarchy | Add coroutine-aware Event subclass (conditional) |
| `libx++/xpp/promise.cpp` | Implementation | Add coroutine resumption logic (conditional) |

### To Create

| File | Purpose |
|------|---------|
| `libx++/xpp/promise_coro.h` (optional) | Coroutine-specific definitions (could be inline in promise.h) |
| `libx++/xpp/promise_coro_test.cpp` | Coroutine test suite |

---

## Appendix: Design Decisions Summary

| Aspect | Current (Phase 1) | Phase 2 Approach |
|--------|-------------------|-----------------|
| **Suspension** | Event + blocking loop | Coroutine frame + event callback |
| **Chain allocation** | Heap-per-then() | Coroutine frame (or existing then() for non-coro paths) |
| **Type erasure** | `read(void*dest)` | Same, but called from `await_resume()` |
| **Void handling** | `struct Void{}` | Template specialization for `void` |
| **Thread model** | Single + WaitScope | Same (one loop per thread) |
| **C++ version** | C++11 | C++11 (default) + C++20 optional |
| **Feature detection** | N/A | `XPP_HAS_COROUTINES` macro |
| **Backward compat** | N/A | 100% (no breaking changes) |

