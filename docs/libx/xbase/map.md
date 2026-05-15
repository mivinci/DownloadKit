# map.h — Generic Key-Value Map

## Introduction

`map.h` provides a generic associative container that stores opaque key-value pairs and supports multiple backend implementations selected at creation time. Users supply a hash function and an equality function; the map handles collision resolution, resizing, and iteration internally. Three backends are available: separate-chaining hash table, open-addressing hash table, and red-black tree.

## Design Philosophy

1. **vtable-Driven Polymorphism** — All backends share a common `xMapVTable` dispatch table. The public API (`xMapSet`, `xMapGet`, `xMapDel`, etc.) forwards calls through function pointers, so callers can switch backends by changing a single `xMapType` argument without touching any other code.

2. **Opaque Keys and Values** — The map stores `const void *` keys and `void *` values. Hash and equality functions are user-supplied, making the map reusable for any key type (strings, integers, structs) without code generation or macros.

3. **Single-Allocation Construction** — The hash and flat backends allocate the struct header and the initial bucket/slot array in one contiguous `calloc` call. This reduces allocation overhead and improves cache locality for small maps.

4. **No Key/Value Ownership** — The map does not own the keys or values it stores. `xMapDestroy()` frees internal structures but NOT user data. This gives the caller full control over element lifecycle.

5. **Built-in Hash Helpers** — Common hash/equality pairs for C strings (`xMapStrHash` / `xMapStrEq`) and integer keys (`xMapIntHash` / `xMapIntEq`) are provided out of the box, covering the two most frequent use cases.

## Architecture

```mermaid
graph TD
    CREATE["xMapCreate(type, cap, hash, eq)"]
    HASH["xMapType_Hash<br/>Separate Chaining"]
    FLAT["xMapType_Flat<br/>Open Addressing"]
    TREE["xMapType_Tree<br/>Red-Black Tree"]

    CREATE -->|"type = Hash"| HASH
    CREATE -->|"type = Flat"| FLAT
    CREATE -->|"type = Tree"| TREE

    API["Public API<br/>Set / Get / Del / Len / Iterate"]

    HASH --> VT["xMapVTable dispatch"]
    FLAT --> VT
    TREE --> VT
    VT --> API

    style CREATE fill:#4a90d9,color:#fff
    style HASH fill:#f5a623,color:#fff
    style FLAT fill:#50b86c,color:#fff
    style TREE fill:#e74c3c,color:#fff
    style API fill:#4a90d9,color:#fff
```

### Internal Dispatch

```mermaid
graph LR
    subgraph "xMapBase (common header)"
        VTABLE["vtable *"]
        HASHFN["hash()"]
        EQFN["eq()"]
    end

    subgraph "xMapVTable"
        SET["set()"]
        GET["get()"]
        DEL["del()"]
        LEN["len()"]
        ITER["iterate()"]
        DESTROY["destroy()"]
    end

    VTABLE --> SET
    VTABLE --> GET
    VTABLE --> DEL
    VTABLE --> LEN
    VTABLE --> ITER
    VTABLE --> DESTROY
```

Every backend struct embeds `xMapBase` as its first member. The public API casts the opaque `xMap` handle to `xMapBase *` to access the vtable, then dispatches to the backend-specific implementation.

## Backend Implementations

### Hash (Separate Chaining)

```text
┌─────────────────────────────────────────┐
│ xMapHash (single calloc)               │
│   base: { vtable, hash, eq }           │
│   buckets → ┌──┬──┬──┬──┬──┬──┐       │
│             │  │  │  │  │  │  │ ...    │
│             └──┴──┴──┴──┴──┴──┘       │
│   size, cap                             │
└─────────────────────────────────────────┘
         │
         ▼
   ┌─────────┐    ┌─────────┐
   │ Entry   │───▶│ Entry   │───▶ NULL
   │ key,val │    │ key,val │
   └─────────┘    └─────────┘
```

- **Collision resolution:** Linked list per bucket.
- **Load factor threshold:** 75% — triggers 2× resize with full rehash.
- **Memory layout:** Initial buckets are allocated inline (contiguous with the struct). After the first resize, buckets are a separate allocation.
- **Best for:** General-purpose use, pointer-heavy keys, high collision tolerance.

### Flat (Open Addressing, Linear Probing)

```text
┌─────────────────────────────────────────┐
│ xMapFlat (single calloc)               │
│   base: { vtable, hash, eq }           │
│   slots → ┌───────┬───────┬───────┐    │
│           │ key   │ key   │ EMPTY │... │
│           │ val   │ val   │       │    │
│           │ OCCUP │ OCCUP │       │    │
│           └───────┴───────┴───────┘    │
│   size, cap                             │
└─────────────────────────────────────────┘
```

- **Collision resolution:** Linear probing with tombstone markers for deletion.
- **Load factor threshold:** 70% — triggers 2× resize (tombstones are discarded during rehash).
- **Slot states:** `EMPTY` (never used), `OCCUPIED` (active entry), `TOMBSTONE` (deleted, probe continues).
- **Memory layout:** Initial slots are allocated inline. After the first resize, slots are a separate allocation.
- **Best for:** Small keys (integers, pointers), cache-friendly sequential access, iteration-heavy workloads.

### Tree (Red-Black Tree)

```text
         ┌───────────┐
         │  node(B)  │
         │ hash=500  │
         ├─────┬─────┤
         │     │     │
    ┌────▼──┐ ┌▼────────┐
    │node(R)│ │ node(R)  │
    │hash=200│ │ hash=800 │
    └───────┘ └──────────┘
```

- **Ordering:** Nodes are ordered by 64-bit hash value.
- **Hash collisions:** When two different keys produce the same hash, the first key is stored in the tree node's primary slot; additional keys are chained in a singly-linked overflow list (`xTreeOverflow`).
- **Deletion optimization:** When deleting a primary key that has overflow entries, the first overflow entry is promoted to primary — avoiding an expensive RB-tree fixup.
- **No pre-allocation:** The `cap` parameter is ignored; nodes are allocated individually on insert.
- **Best for:** Ordered iteration by hash value, worst-case O(log n) guarantees, workloads where hash table resizing pauses are unacceptable.

## Operations and Complexity

| Operation | Hash (avg) | Hash (worst) | Flat (avg) | Flat (worst) | Tree |
| --- | --- | --- | --- | --- | --- |
| `xMapSet` | O(1) | O(n) | O(1) | O(n) | O(log n) |
| `xMapGet` | O(1) | O(n) | O(1) | O(n) | O(log n) |
| `xMapDel` | O(1) | O(n) | O(1) | O(n) | O(log n) |
| `xMapLen` | O(1) | O(1) | O(1) | O(1) | O(1) |
| `xMapIterate` | O(n + cap) | O(n + cap) | O(cap) | O(cap) | O(n) |
| `xMapCreate` | O(cap) | O(cap) | O(cap) | O(cap) | O(1) |
| `xMapDestroy` | O(n + cap) | O(n + cap) | O(cap) | O(cap) | O(n) |

> **Note:** For Hash, iteration visits all buckets (including empty ones). For Flat, iteration visits all slots. Tree iteration is a pure in-order traversal visiting only occupied nodes.

## API Reference

### Types

| Type | Description |
| --- | --- |
| `xMapType` | Enum: `xMapType_Hash` (separate chaining), `xMapType_Flat` (open addressing), `xMapType_Tree` (red-black tree) |
| `xMap` | Opaque handle to a map |
| `xMapHashFunc` | `uint64_t (*)(const void *key)` — Returns a 64-bit hash for the given key |
| `xMapEqFunc` | `bool (*)(const void *a, const void *b)` — Returns true if two keys are equal |
| `xMapIterFunc` | `bool (*)(const void *key, void *val, void *arg)` — Iterator callback; return false to stop early |

### Functions

| Function | Signature | Description | Thread Safety |
| --- | --- | --- | --- |
| `xMapCreate` | `xMap xMapCreate(xMapType type, size_t cap, xMapHashFunc hash, xMapEqFunc eq)` | Create a map with the specified backend. `cap = 0` uses default (16). `hash` and `eq` are required. | Not thread-safe |
| `xMapDestroy` | `void xMapDestroy(xMap m)` | Free the map. Does NOT free user keys/values. NULL is a safe no-op. | Not thread-safe |
| `xMapSet` | `xErrno xMapSet(xMap m, const void *key, void *val)` | Insert or update a key-value pair. Returns `xErrno_Ok` or `xErrno_NoMemory`. | Not thread-safe |
| `xMapGet` | `void *xMapGet(xMap m, const void *key)` | Look up a value by key. Returns NULL if not found. | Not thread-safe |
| `xMapDel` | `void *xMapDel(xMap m, const void *key)` | Remove a key-value pair. Returns the removed value, or NULL. | Not thread-safe |
| `xMapLen` | `size_t xMapLen(xMap m)` | Return the number of entries. O(1). | Not thread-safe |
| `xMapIterate` | `void xMapIterate(xMap m, xMapIterFunc fn, void *arg)` | Iterate over all entries. Callback returns false to stop early. | Not thread-safe |

### Built-in Hash / Equality Helpers

| Function | Description |
| --- | --- |
| `xMapStrHash` | FNV-1a 64-bit hash for NUL-terminated C strings |
| `xMapStrEq` | `strcmp`-based equality for C strings |
| `xMapIntHash` | Splitmix64 finalizer for integer keys cast to `(void *)` |
| `xMapIntEq` | Pointer-value equality for integer keys cast to `(void *)` |

## Usage Examples

### String-Keyed Map

```c
#include <stdio.h>
#include <xbase/map.h>

int main(void) {
    xMap m = xMapCreate(xMapType_Hash, 0, xMapStrHash, xMapStrEq);

    xMapSet(m, "alice", (void *)"engineer");
    xMapSet(m, "bob",   (void *)"designer");
    xMapSet(m, "carol", (void *)"manager");

    printf("alice = %s\n", (const char *)xMapGet(m, "alice"));
    printf("bob   = %s\n", (const char *)xMapGet(m, "bob"));

    // Update existing key
    xMapSet(m, "alice", (void *)"senior engineer");
    printf("alice = %s\n", (const char *)xMapGet(m, "alice"));

    // Delete
    xMapDel(m, "bob");
    printf("bob   = %s\n", xMapGet(m, "bob") ? "found" : "not found");
    printf("len   = %zu\n", xMapLen(m));

    xMapDestroy(m);
    return 0;
}
```

### Integer-Keyed Map with Iteration

```c
#include <stdio.h>
#include <xbase/map.h>

static bool print_entry(const void *key, void *val, void *arg) {
    (void)arg;
    printf("  key=%ld val=%ld\n", (long)(intptr_t)key, (long)(intptr_t)val);
    return true; // continue iteration
}

int main(void) {
    // Use flat map for cache-friendly integer lookups
    xMap m = xMapCreate(xMapType_Flat, 0, xMapIntHash, xMapIntEq);

    for (int i = 1; i <= 10; i++) {
        xMapSet(m, (const void *)(intptr_t)i,
                   (void *)(intptr_t)(i * i));
    }

    printf("Entries (%zu):\n", xMapLen(m));
    xMapIterate(m, print_entry, NULL);

    xMapDestroy(m);
    return 0;
}
```

### Choosing a Backend

```c
#include <xbase/map.h>

void example(void) {
    // General purpose — good default
    xMap hash_map = xMapCreate(xMapType_Hash, 0, xMapStrHash, xMapStrEq);

    // Cache-friendly for small integer keys
    xMap flat_map = xMapCreate(xMapType_Flat, 0, xMapIntHash, xMapIntEq);

    // Ordered iteration, O(log n) worst-case guarantees
    xMap tree_map = xMapCreate(xMapType_Tree, 0, xMapStrHash, xMapStrEq);

    // ... use them identically via xMapSet/xMapGet/xMapDel ...

    xMapDestroy(hash_map);
    xMapDestroy(flat_map);
    xMapDestroy(tree_map);
}
```

## How to Choose a Backend

| Criteria | Hash | Flat | Tree |
| --- | --- | --- | --- |
| **Average lookup** | O(1) ✅ | O(1) ✅ | O(log n) |
| **Worst-case lookup** | O(n) | O(n) | O(log n) ✅ |
| **Cache locality** | Poor (pointer chasing) | Excellent ✅ | Poor (pointer chasing) |
| **Iteration speed** | Visits empty buckets | Visits empty slots | Visits only entries ✅ |
| **Ordered iteration** | No | No | Yes (by hash) ✅ |
| **Resize pauses** | Yes (rehash) | Yes (rehash) | No ✅ |
| **Memory overhead** | Entry nodes + bucket array | Slot array (inline) ✅ | Node + parent/child pointers |
| **Deletion** | Free entry node | Tombstone marker | RB fixup or overflow promotion |
| **Best for** | General purpose | Small keys, hot loops | Ordered access, latency-sensitive |

**Rule of thumb:** Start with `xMapType_Hash`. Switch to `xMapType_Flat` if profiling shows cache misses dominate. Use `xMapType_Tree` when you need ordered iteration or cannot tolerate resize pauses.

## Use Cases

1. **Session Management** — Store active sessions keyed by session ID (string). The hash backend provides O(1) average lookup for connection dispatch.

2. **Configuration Registry** — Map string keys to configuration values. The tree backend provides ordered iteration for serialization.

3. **Object Caches** — Cache computed results keyed by integer IDs. The flat backend's cache-friendly layout minimizes latency for hot-path lookups.

4. **Symbol Tables** — Compilers and interpreters can use the map to store variable bindings, with string keys and pointer values.

## Best Practices

- **Always provide both `hash` and `eq`.** The map requires both functions; passing NULL for either causes `xMapCreate` to return NULL.
- **Use the built-in helpers when possible.** `xMapStrHash`/`xMapStrEq` and `xMapIntHash`/`xMapIntEq` are well-tested and optimized.
- **Keys must remain valid while stored.** The map stores key pointers, not copies. If you free a key while it's in the map, lookups will read freed memory.
- **Don't modify keys in-place.** Changing a key's content after insertion will corrupt the map's internal structure (wrong bucket/slot/tree position).
- **Pre-size when the count is known.** Pass a `cap` hint to `xMapCreate` to avoid early resizes. For hash and flat backends, capacity should be a power of 2.
- **Prefer `xMapType_Hash` as the default.** It handles the widest range of workloads well. Only switch backends based on profiling data.

## Comparison with Other Libraries

| Feature | xbase map.h | C++ `std::unordered_map` | Go `map` | GLib `GHashTable` | uthash |
| --- | --- | --- | --- | --- | --- |
| **Language** | C99 | C++ | Go | C | C (macros) |
| **Key Type** | `void *` (generic) | Template | `comparable` | `gpointer` | Struct field |
| **Multiple Backends** | Hash / Flat / Tree ✅ | Hash only | Hash only | Hash only | Hash only |
| **Ordered Iteration** | Tree backend ✅ | No (`std::map` for ordered) | No | No | No |
| **Ownership** | No (caller owns) | Yes (copies) | Yes (copies) | No | No |
| **Thread Safety** | Not thread-safe | Not thread-safe | Not thread-safe | Not thread-safe | Not thread-safe |
| **Resize Strategy** | 2× with rehash | Bucket-based rehash | Incremental | Bucket-based rehash | Bucket-based rehash |
| **Intrusive** | No | No | No | No | Yes (struct embedding) |

**Key Differentiator:** xbase's map provides three interchangeable backends behind a single API. Callers can tune the data structure to their workload (cache locality, ordered access, worst-case guarantees) without changing any code beyond the `xMapType` argument.

## Benchmark

> Environment: Apple Mac15,7 (12 cores), 36 GB RAM, macOS 26.x, Release build (`-O2`). Each result is the median of 3 repetitions (`--benchmark_min_time=0.5s --benchmark_repetitions=3`).
> Source: [`xbase/map_bench.cpp`](https://github.com/mivinci/moo/blob/main/libx/xbase/map_bench.cpp)
>
> The hash and tree backends allocate nodes through `xSlab` (see [slab.md](slab.md)); the flat backend uses a single contiguous array and does no per-entry allocation.

### Set (Insert)

| Benchmark | N | Time (ns) | CPU (ns) | Throughput |
| --- | ---: | ---: | ---: | --- |
| `BM_Map_Set_Hash` | 64 | 4,879 | 4,879 | 13.1 M items/s |
| `BM_Map_Set_Hash` | 512 | 9,027 | 9,027 | 56.7 M items/s |
| `BM_Map_Set_Hash` | 4,096 | 56,781 | 56,779 | 72.1 M items/s |
| `BM_Map_Set_Hash` | 32,768 | 713,860 | 713,810 | 45.9 M items/s |
| `BM_Map_Set_Flat` | 64 | 1,061 | 1,062 | 60.2 M items/s |
| `BM_Map_Set_Flat` | 512 | 5,507 | 5,508 | 93.0 M items/s |
| `BM_Map_Set_Flat` | 4,096 | 48,033 | 48,036 | 85.3 M items/s |
| `BM_Map_Set_Flat` | 32,768 | 689,267 | 689,275 | 47.5 M items/s |
| `BM_Map_Set_Tree` | 64 | 5,265 | 5,268 | 12.1 M items/s |
| `BM_Map_Set_Tree` | 512 | 11,232 | 11,233 | 45.6 M items/s |
| `BM_Map_Set_Tree` | 4,096 | 146,120 | 146,120 | 28.0 M items/s |
| `BM_Map_Set_Tree` | 32,768 | 3,154,728 | 3,154,598 | 10.4 M items/s |

### Get (Lookup)

| Benchmark | N | Time (ns) | CPU (ns) | Throughput |
| --- | ---: | ---: | ---: | --- |
| `BM_Map_Get_Hash` | 64 | 214 | 214 | 298.7 M items/s |
| `BM_Map_Get_Hash` | 512 | 1,967 | 1,967 | 260.3 M items/s |
| `BM_Map_Get_Hash` | 4,096 | 20,192 | 20,187 | 202.9 M items/s |
| `BM_Map_Get_Hash` | 32,768 | 207,804 | 207,791 | 157.7 M items/s |
| `BM_Map_Get_Flat` | 64 | 243 | 243 | 263.8 M items/s |
| `BM_Map_Get_Flat` | 512 | 2,276 | 2,276 | 224.9 M items/s |
| `BM_Map_Get_Flat` | 4,096 | 22,258 | 22,256 | 184.0 M items/s |
| `BM_Map_Get_Flat` | 32,768 | 256,893 | 256,885 | 127.6 M items/s |
| `BM_Map_Get_Tree` | 64 | 438 | 438 | 146.1 M items/s |
| `BM_Map_Get_Tree` | 512 | 4,829 | 4,829 | 106.0 M items/s |
| `BM_Map_Get_Tree` | 4,096 | 60,687 | 60,687 | 67.5 M items/s |
| `BM_Map_Get_Tree` | 32,768 | 2,600,910 | 2,600,792 | 12.6 M items/s |

### Del (Delete)

| Benchmark | N | Time (ns) | CPU (ns) | Throughput |
| --- | ---: | ---: | ---: | --- |
| `BM_Map_Del_Hash` | 64 | 1,247 | 1,250 | 51.2 M items/s |
| `BM_Map_Del_Hash` | 512 | 3,366 | 3,371 | 151.9 M items/s |
| `BM_Map_Del_Hash` | 4,096 | 23,818 | 23,814 | 172.0 M items/s |
| `BM_Map_Del_Hash` | 32,768 | 209,060 | 209,018 | 156.8 M items/s |
| `BM_Map_Del_Flat` | 64 | 1,153 | 1,155 | 55.4 M items/s |
| `BM_Map_Del_Flat` | 512 | 3,026 | 3,030 | 169.0 M items/s |
| `BM_Map_Del_Flat` | 4,096 | 21,236 | 21,243 | 192.8 M items/s |
| `BM_Map_Del_Flat` | 32,768 | 270,593 | 268,020 | 122.3 M items/s |
| `BM_Map_Del_Tree` | 64 | 1,788 | 1,791 | 35.7 M items/s |
| `BM_Map_Del_Tree` | 512 | 8,524 | 8,527 | 60.0 M items/s |
| `BM_Map_Del_Tree` | 4,096 | 146,494 | 145,907 | 28.1 M items/s |
| `BM_Map_Del_Tree` | 32,768 | 2,672,192 | 2,672,155 | 12.3 M items/s |

### Iterate

| Benchmark | N | Time (ns) | CPU (ns) | Throughput |
| --- | ---: | ---: | ---: | --- |
| `BM_Map_Iterate_Hash` | 64 | 128 | 128 | 500.2 M items/s |
| `BM_Map_Iterate_Hash` | 512 | 1,030 | 1,030 | 497.3 M items/s |
| `BM_Map_Iterate_Hash` | 4,096 | 8,436 | 8,436 | 485.5 M items/s |
| `BM_Map_Iterate_Hash` | 32,768 | 169,785 | 169,780 | 193.0 M items/s |
| `BM_Map_Iterate_Flat` | 64 | 120 | 120 | 534.7 M items/s |
| `BM_Map_Iterate_Flat` | 512 | 973 | 973 | 526.0 M items/s |
| `BM_Map_Iterate_Flat` | 4,096 | 7,775 | 7,774 | 526.9 M items/s |
| `BM_Map_Iterate_Flat` | 32,768 | 113,315 | 113,308 | 289.2 M items/s |
| `BM_Map_Iterate_Tree` | 64 | 154 | 154 | 416.7 M items/s |
| `BM_Map_Iterate_Tree` | 512 | 1,235 | 1,235 | 414.4 M items/s |
| `BM_Map_Iterate_Tree` | 4,096 | 10,813 | 10,812 | 378.8 M items/s |
| `BM_Map_Iterate_Tree` | 32,768 | 178,903 | 178,901 | 183.2 M items/s |

**Key Observations:**

- **Flat is fastest for small maps.** At N≤512, flat's contiguous array layout beats hash on both insert and iterate, and trades evenly with hash on lookup/delete. It is the right choice when capacity fits in a few cache lines.
- **Hash scales better at large N.** At N=32K, hash sustains **157.7 M lookups/s** vs flat's **127.6 M** and tree's **12.6 M** — separate-chaining avoids the probe-length blowup that hurts flat as load increases.
- **Tree pays for ordering.** At N=32K, tree set throughput is **10.4 M items/s** (~30× slower than flat). Pick tree only when range scans or predictable worst-case latency matter; its iterate throughput remains strong at small N because the red-black walk stays cache-resident.
- **Iteration dominates everywhere.** Flat peaks at ~535 M items/s (pure sequential scan), hash ~500 M (bucket hop + chain), tree ~415 M (in-order recursion). Use iterate for bulk scans rather than repeatedly calling `xMapGet`.
- **Large-N drops are real.** Both flat and hash lose roughly a third of peak throughput between 4K and 32K entries — this is the L2-to-L3 cache boundary, not an algorithmic issue.
