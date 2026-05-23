# Phase 2: C++20 Coroutine Integration for libx++ Promise<T>

## 📚 Documentation Index

This directory contains a comprehensive analysis and implementation roadmap for Phase 2 of the libx++ async runtime, which adds C++20 coroutine (`co_await`) support to the existing Promise<T> system.

### Quick Start (Pick One)

**If you have 5 minutes:**
- Read: `PHASE2_SUMMARY.txt` - Executive overview and next steps

**If you have 30 minutes:**
- Read: `QUICK_REFERENCE.md` - Concise summary with code examples
- Skim: `ARCHITECTURE_DIAGRAM.txt` - Visual overview

**If you have 2 hours:**
- Read: `PHASE2_ANALYSIS.md` - Deep dive into current implementation
- Review: `IMPLEMENTATION_ROADMAP.md` - Concrete implementation plan

**If you're ready to implement:**
- Follow: `IMPLEMENTATION_ROADMAP.md` - Stage-by-stage guide with code
- Reference: `PHASE2_ANALYSIS.md` Part 1 - Detailed component breakdown

---

## 📋 Document Guide

### 1. PHASE2_SUMMARY.txt (11 KB)
**Best for**: Getting the lay of the land

Contains:
- Overview of all 4 generated documents
- Key findings (architecture readiness, implementation quality, etc.)
- Current state summary (line-by-line of each file)
- Deliverables and timeline
- Why this matters (performance & developer experience)
- Next steps

**Start here if:** You need a quick understanding of the project scope

---

### 2. QUICK_REFERENCE.md (11 KB)
**Best for**: Developers who want concise facts

Contains:
- TL;DR summary with code examples
- One-liner component summaries (9 components)
- Why coroutines win (side-by-side comparison)
- File-by-file review (promise.h, promise_node.h, etc.)
- Architecture model
- Phase 2 checklist (actionable items)
- Integration points
- Real-world example (HTTP client before/after)
- Design philosophy

**Start here if:** You're familiar with promises and want quick answers

---

### 3. ARCHITECTURE_DIAGRAM.txt (22 KB)
**Best for**: Visual learners

Contains:
- ASCII diagrams of:
  - Public API overview
  - Internal node hierarchy
  - Event classes
  - Control flow (.wait() execution)
  - Promise chaining example
  - Heap allocation pattern
  - Promise flattening mechanism

**Start here if:** You prefer diagrams to prose

---

### 4. PHASE2_ANALYSIS.md (20 KB)
**Best for**: Comprehensive understanding

Contains (7 detailed parts):
1. Executive summary
2. **Part 1: Current Promise<T> Implementation** (most important)
   - Promise<T> class structure
   - Static factories (resolve, eval, make)
   - Chaining API (.then())
   - Synchronous wait (.wait())
3. **Part 2: C++ Standard Version & Compiler Support**
   - Current: C++11 only
   - No existing coroutine usage
   - Phase 2 must use opt-in conditional compilation
4. **Part 3: Test Coverage & API Surface**
   - 22 existing tests covering all major functionality
5. **Part 4: Architecture Decisions & Integration Points**
   - Why coroutines fit naturally
   - Proposed coroutine interface
   - Compiler detection strategy
6. **Part 5: Key Integration Points & Challenges**
   - WaitScope integration
   - Void type handling
   - Type erasure in await_resume()
   - State management with ChainPromiseNode
7. **Part 6: Incremental Migration Path**
   - Phase 2 milestones (11-18 days total)
   - Backward compatibility guarantees
8. **Part 7: Key Files & Locations**
   - Files to modify
   - Files to create

**Start here if:** You need deep technical understanding

---

### 5. IMPLEMENTATION_ROADMAP.md (16 KB)
**Best for**: Developers implementing Phase 2

Contains (6 stages with concrete code):

**Stage 1: Compiler Detection (1-2 days)**
- Add `XPP_HAS_COROUTINES` to compiler.h
- Update CMakeLists.txt

**Stage 2: Awaitable Methods (2-3 days)**
- Add `await_ready()`, `await_suspend()`, `await_resume()` to promise.h
- Handle Promise<void> specialization
- Include <coroutine> header guard

**Stage 3: CoroutineEvent Implementation (2-3 days)**
- Add CoroutineEvent class to promise_node.h
- Stores std::coroutine_handle<>
- Calls handle.resume() when fired

**Stage 4: Integration Tests (3-5 days)**
- Create promise_coro_test.cpp
- 7 test cases covering all major scenarios
- Update CMakeLists.txt

**Stage 5: Optimization & Documentation (2-3 days)**
- Benchmark script comparing .then() vs coroutines
- Documentation examples

**Stage 6: Testing Checklist**
- Functional tests (7 items)
- Edge cases (5 items)
- Performance verification (4 items)
- Compatibility verification (4 items)

Plus:
- Implementation order (11-18 day timeline)
- Backward compatibility verification

**Start here if:** You're ready to write code

---

## 🎯 Key Findings

### ✅ Architecture is Ready
The poll/event design was explicitly built for coroutines. The mapping is natural:
```
poll()         → await_suspend(handle)
Event::fire()  → std::coroutine_handle<>::resume()
read()         → await_resume()
```

### ✅ Phase 1 Quality is Excellent
- Move-only semantics prevent use-after-free
- Virtual node hierarchy for type-generic event handling
- Clever ChainPromiseNode for Promise<Promise<T>> flattening
- Thread-local event loop integration
- 22 comprehensive tests

### ✅ Zero Compilation Impact
- Base library stays C++11
- Coroutine support is #ifdef guarded
- C++11/C++17 consumers see no difference

### ⚠️ One Design Note
`await_suspend()` allocates CoroutineEvent on heap (consistent with .then() pattern).
May benefit from arena allocation in Phase 3, but not a blocker for Phase 2.

---

## 📊 Timeline

**Total Duration**: 11-18 days

```
Week 1:
  Mon-Tue:  Compiler detection (2 days)
  Wed-Thu:  Awaitable methods (2 days)
  Fri:      CoroutineEvent (1 day)

Week 2:
  Mon-Wed:  Comprehensive tests (3 days)
  Thu-Fri:  Benchmarks & docs (2 days)

Week 3+:   Optimization & profiling (as needed)
```

---

## 🔍 File Structure

```
libx++/xpp/
├── promise.h           [MODIFY] Add await_* methods
├── promise_node.h      [MODIFY] Add CoroutineEvent
├── promise.cpp         [MODIFY if needed] Event loop integration
├── compiler.h          [MODIFY] Add XPP_HAS_COROUTINES
├── CMakeLists.txt      [MODIFY] C++20 conditional test
├── promise_test.cpp    [EXISTING] 22 Phase 1 tests
├── promise_coro_test.cpp [CREATE] New coroutine tests
└── bench_coroutine.cpp [CREATE] Optional benchmarks
```

---

## ✨ Why This Matters

### Performance
- Eliminates heap allocation per chain step in coroutine code
- Coroutine frame on stack instead of linked list of transform nodes
- Better cache locality

### Developer Experience
```cpp
// Sequential, readable async code
auto user = co_await fetch_user(id);
auto posts = co_await fetch_posts(user.id);
auto comments = co_await fetch_comments(posts[0].id);

// Instead of nested callbacks
fetch_user(id, [](auto user) {
  fetch_posts(user.id, [](auto posts) {
    fetch_comments(posts[0].id, [](auto comments) {
      // Nested callback hell
    });
  });
});
```

### Debugging
- Coroutine frames appear on the stack
- Exception handling with try/catch works naturally

---

## 🚀 Getting Started

### Step 1: Understand
1. Read PHASE2_SUMMARY.txt (15 min)
2. Skim QUICK_REFERENCE.md (20 min)
3. Review ARCHITECTURE_DIAGRAM.txt (15 min)

### Step 2: Deep Dive
- Read PHASE2_ANALYSIS.md Part 1 (current implementation)

### Step 3: Implement
- Follow IMPLEMENTATION_ROADMAP.md stage by stage
- Start with Stage 1 (simplest, lowest risk)
- Use provided code snippets as templates

### Step 4: Verify
- Run existing tests (should all pass)
- Add new coroutine tests from Stage 4
- Run backward compatibility checks

---

## 📝 Notes

- All code examples in IMPLEMENTATION_ROADMAP.md are ready to copy/paste
- Exact line numbers provided for each modification
- Feature detection via `#ifdef XPP_HAS_COROUTINES` guards all new code
- 100% backward compatible (C++11/C++17 code unaffected)

---

## ❓ FAQ

**Q: Do I have to upgrade to C++20 to use libx++?**
A: No. The library stays C++11. Coroutine support is optional and only available when including with C++20 enabled.

**Q: Will this break existing Promise code?**
A: No. All existing `.then().wait()` code continues to work unchanged.

**Q: How much faster will coroutines be?**
A: Depends on the chain length. Each step eliminated is roughly 1 heap allocation + 1 vtable call. Expect 10-30% speedup in microbenchmarks, depends on profiling.

**Q: Can I mix .then() and co_await?**
A: Yes. You can call .then() on a promise, then co_await the result. The architecture supports mixed usage.

**Q: What if my compiler doesn't support C++20?**
A: The feature macro XPP_HAS_COROUTINES will be 0, and all coroutine code is #ifdef guarded out. No compilation errors.

---

## 📞 Questions?

Refer to:
- PHASE2_ANALYSIS.md Part 5 for integration challenges
- QUICK_REFERENCE.md for one-liner summaries
- ARCHITECTURE_DIAGRAM.txt for visual explanations
- IMPLEMENTATION_ROADMAP.md for concrete code

---

**Status**: Ready for Phase 2 implementation
**Estimated Effort**: 11-18 days
**Risk Level**: Low (fully backward compatible, opt-in feature)
**Priority**: High (eliminates key performance bottleneck)
