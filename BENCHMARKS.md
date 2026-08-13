# VCache benchmarks

Every number here was measured, not estimated. Discovery Document §16 asks for exactly that, and
several of these results contradict assumptions made earlier in the project — those are called out
rather than quietly dropped.

## Machine and method

| | |
|---|---|
| Machine | Apple M3 Pro, 11 cores (5 performance + 6 efficiency), 18 GB RAM |
| OS / compiler | macOS 26.3.1, Apple clang 17.0.0 |
| Build | `-O2`, Release, no sanitizers |
| Reproduce | `cmake -S . -B build && cmake --build build -j && ./build/bench/bench_hashtable` (and `bench_database`, `bench_server`) |

Method, and why:

- **Warmup pass before every measurement.** The first pass through cold caches, an unfaulted heap
  and an unpredicted branch measures startup, not steady state.
- **Median of repetitions**, not mean or minimum. A mean lets one scheduler hiccup dominate; a
  minimum reports the luckiest run.
- **`doNotOptimize()` barriers** around results, so the optimiser cannot delete the work being
  timed — otherwise you measure dead-code elimination and call it a database.
- **Fixed RNG seed**, so every implementation sees identical keys in identical order.

**The M3 Pro has both performance and efficiency cores.** macOS may place a thread on either, so
multi-threaded rows are not perfectly monotonic and should be read as trends, not exact figures.

---

## 1. Custom hash table vs `std::unordered_map`

500,000 keys, 64-byte values, median of 5 runs.

| Workload | VCache | `std::unordered_map` | Ratio |
|---|---:|---:|---:|
| Insert | 2,729,030 ops/s | 4,190,600 ops/s | **0.65×** |
| Insert (pre-sized) | 3,827,368 ops/s | 4,845,198 ops/s | **0.79×** |
| Lookup, hit | 18,194,803 ops/s | 26,172,757 ops/s | **0.69×** |
| Lookup, **miss** | 17,660,434 ops/s | 7,484,647 ops/s | **2.36×** |
| Insert + delete | 6,013,859 ops/s | 7,280,816 ops/s | 0.83× |

**The custom table loses on inserts and hits, and wins decisively on misses.**

The loss is not mysterious. VCache's node is **96 bytes**; libc++'s is roughly 64. The extra 32
carry the TTL expiration, the two recency-list pointers and the cached footprint — features
`unordered_map` has no equivalent of. A 50% larger node means fewer nodes per cache line, and on
a workload that is almost entirely cache misses, that is the dominant cost. VCache is doing
strictly more work per entry and paying for it.

The miss win is the flip side of a Phase 1 decision. Bucket selection is `hash & (n - 1)` because
the bucket count is a power of two; libc++ uses a prime bucket count and therefore a 64-bit
modulo — tens of cycles per lookup. On a hit, that arithmetic is hidden behind the node fetch. On
a miss there is often no node to fetch, so the division is exposed and dominates.

**Honest reading:** hand-writing the table did not produce a faster general-purpose hash map, and
claiming otherwise would be false. It produced one that is competitive, is 2.4× faster on the
negative-lookup path a cache hits constantly, and carries TTL and LRU metadata inline — which is
what actually made Phases 6 and 8 possible without a second data structure.

### Hash distribution

| | |
|---|---|
| Entries | 500,000 |
| Buckets | 1,048,576 |
| Load factor | 0.477 |
| **Longest chain** | **7** |

With 500,000 keys, no chain exceeds 7. The FNV-1a + splitmix64 avalanche is spreading keys
evenly, so the O(1) claim holds in practice and not just on paper.

---

## 2. Memory accounting accuracy

200,000 entries, ~28-byte keys, 200-byte values.

| | |
|---|---:|
| VCache estimate | 77,674,039 bytes |
| Actual RSS delta | 45,072,384 bytes |
| **Estimate / actual** | **1.72×** |

**The Phase 8 accounting over-counts by about 72%.** The per-entry model is
`sizeof(Node) + 48 + key.size() + value.size()`, and that flat 48-byte allocator allowance is too
generous — it charges for heap allocations that the small-string optimisation avoids, and ignores
that the allocator's real overhead per block is smaller than assumed.

This is a real inaccuracy, and it means `--max-memory 100MB` currently starts evicting at roughly
58 MB of true footprint. It is conservative rather than dangerous — the server under-uses memory
instead of overshooting the limit — but it is wrong, and calibrating the constant against this
measurement is the obvious next fix.

> A methodology note worth recording: the first version of this measurement used
> `getrusage(ru_maxrss)`, which reports a **peak**. Since earlier benchmarks in the same process
> had already peaked higher, the delta read as meaningless. It now reads current RSS via mach
> `task_info`.

---

## 3. Lock strategy — testing the Phase 5 decision

90% reads / 10% writes, 100,000 keys, 200,000 ops per thread, median of 3. Figures are ops/sec.

| Strategy | 1 thread | 2 threads | 4 threads | 8 threads |
|---|---:|---:|---:|---:|
| No lock (unsafe ceiling) | 5,072,628 | – | – | – |
| `std::mutex` | 3,551,594 | 2,433,279 | 789,855 | 1,677,148 |
| **`std::shared_mutex` (shared reads)** | 3,765,004 | **3,912,883** | 2,265,757 | 1,063,014 |
| `std::shared_mutex` (exclusive always) | 3,560,709 | 2,064,613 | 1,149,454 | 660,065 |

**Phase 5's choice was right — and the reasoning behind it was incomplete.**

Right: the reader/writer lock beats a plain mutex at 1, 2 and 4 threads, by up to 2.9× at four
threads. Comparing the two `shared_mutex` rows isolates the benefit of *sharing* from the lock's
own overhead, and sharing is clearly doing real work.

Incomplete: **nothing scales.** Peak throughput is at *two* threads, and by eight threads every
locked strategy is worse than single-threaded. Even a shared lock atomically increments a reader
counter, so every reader writes to the same cache line and bounces it between cores. A
reader/writer lock removes *mutual exclusion* between readers; it does not remove *contention*.

At 8 threads the plain mutex actually overtakes `shared_mutex` (1.68M vs 1.06M) — under heavy
contention, the simpler lock's cheaper state wins.

**What this means:** the note in `Database.h` saying Phase 9 should also measure per-shard locks
was the right instinct, and sharding is the real fix. A single global lock is the ceiling on this
design regardless of which lock it is.

---

## 4. The cost of returning values by copy — testing the Phase 2 decision

100,000 keys. Both paths take the same shared lock, so only the copy differs.

| Value size | Pointer (under lock) | `Database::get` (copies) | Copy cost |
|---|---:|---:|---:|
| 16 B | 24,928,076 ops/s | 21,946,872 ops/s | **12%** |
| 256 B | 27,609,696 ops/s | 11,711,488 ops/s | **57%** |
| 4096 B | 25,096,465 ops/s | 2,671,895 ops/s | **89%** |

**Phase 2's decision is cheap for small values and brutal for large ones.** At 16 bytes the copy
costs 12% — a fair price for a signature that survived concurrency without modification. At 4 KB
it costs 89% of throughput.

The mitigation documented back in Phase 2 — storing `shared_ptr<const string>` so reads share
ownership instead of copying — is now justified by data rather than by intuition, and it matters
specifically for large-value workloads. For a session-cache workload of short strings, the current
design is fine.

---

## 5. The cost of LRU tracking — testing the Phase 8 decision

GET-only, 100,000 keys, limit set high enough that nothing is actually evicted, so this isolates
the *bookkeeping* from the eviction work. Figures are ops/sec.

| Configuration | 1 thread | 2 threads | 4 threads | 8 threads |
|---|---:|---:|---:|---:|
| No limit (no tracking) | 3,463,793 | 5,263,421 | 2,945,843 | 2,094,945 |
| Limit set (LRU tracking) | 3,144,360 | 3,630,829 | 2,848,431 | 2,027,153 |
| **Cost** | **9%** | **31%** | 3% | 3% |

Enabling eviction costs 9% single-threaded and 31% at two threads — the point where the extra
recency mutex is most visible. Past four threads the difference nearly vanishes, because the main
`shared_mutex` is already the bottleneck and the second lock adds little on top.

Cheaper than feared. The Phase 8 note about Redis using sampled approximate LRU to avoid exactly
this cost still stands as the scalable answer, but the strict list is not the thing limiting this
server today — the global lock is.

---

## 6. End-to-end server throughput and latency

Real TCP over loopback, `TCP_NODELAY`, fixed budget of 200,000 operations split across clients.
Latencies in **microseconds**.

### SET

| Clients | ops/sec | mean | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 64,483 | 15.2 | 14.8 | 18.0 | 28.4 | 8,207.9 |
| 10 | 200,438 | 49.5 | 44.8 | 74.3 | 100.0 | 7,001.8 |
| 100 | 195,214 | 506.3 | 483.4 | 619.5 | 1,167.8 | 12,562.2 |
| 1000 | 160,188 | 6,023.5 | 5,292.0 | 6,984.1 | 27,757.7 | 77,109.0 |

### GET

| Clients | ops/sec | mean | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 66,581 | 15.0 | 14.5 | 17.1 | 23.5 | 776.9 |
| 10 | 213,292 | 46.8 | 43.3 | 68.0 | 85.4 | 884.4 |
| 100 | 215,021 | 464.5 | 459.0 | 536.4 | 565.2 | 824.1 |
| 1000 | 190,089 | 5,133.2 | 4,954.0 | 5,897.2 | 6,407.3 | 19,918.0 |

**Throughput saturates at about 10 clients and roughly 200,000 ops/sec.** Going from 10 to 100
clients buys no throughput at all (200k → 195k for SET) while multiplying latency tenfold. That is
the textbook signature of a saturated system: past saturation, extra concurrency becomes queueing,
not work.

**Latency degrades gracefully, then does not.** GET p99 stays under 600 µs at 100 clients but
reaches 6.4 ms at 1000. SET is worse at 1000 clients (27.8 ms p99) because writes take the
exclusive lock and serialise completely.

**`max` is a poor metric and included only because it is asked for.** The 1-client SET row shows a
p99 of 28 µs and a max of 8.2 ms — a single scheduling outlier, 290× the p99. This is exactly why
§16 asks for percentiles.

### Pipelined (round trip removed)

| Configuration | ops/sec |
|---|---:|
| 1 client, batches of 100 | **652,233** |
| 10 clients, batches of 100 | 388,213 |
| 50 clients, batches of 100 | 259,266 |

**A single pipelined client reaches 652k ops/sec — 10× the un-pipelined single-client figure of
64k.** That gap is not the database; it is one network round trip per command. Roughly 90% of
single-client latency is waiting for the network, not doing work.

And note that pipelined throughput *falls* as clients are added — the same lock-contention wall
measured in §3, now visible end to end. The database layer peaks around two threads, and so does
the server built on it.

---

## 7. Sharding — the fix, and what it bought

Everything above was measured against a single `std::shared_mutex` over the whole keyspace. That
lock is now split into N independently locked shards, keyed by the high bits of the hash. Same
workload, same machine, 90% reads:

| Shards | 1 thread | 2 threads | 4 threads | 8 threads |
|---|---:|---:|---:|---:|
| **1** (the old design) | 2,538,548 | 2,725,126 | 2,162,647 | **1,016,706** |
| 4 | 1,637,006 | 3,687,715 | 2,540,639 | 2,771,167 |
| 16 | 2,089,617 | 4,576,484 | 7,194,973 | 5,108,002 |
| **64** (the new default) | 2,451,218 | 5,896,433 | 8,652,340 | **10,690,837** |

**10.5× at eight threads**, and throughput now climbs with thread count instead of peaking at two
and collapsing. Single-threaded cost is about 4% — one extra shift and an indirection per
operation — which is a trivial price.

End to end, over real TCP with pipelining:

| Pipelined SET | Before | After |
|---|---:|---:|
| 1 client | 652,233 | **812,497** |
| 10 clients | 388,213 | **1,342,224** |
| 50 clients | 259,266 | **1,359,288** |

Before sharding, adding clients made pipelined throughput *worse* — the contention wall, visible
from outside the process. It now scales.

**The un-pipelined request/response numbers barely moved** (~215,000 ops/sec, unchanged). That is
the correct result, not a disappointment: those are bound by one network round trip per command,
which sharding has nothing to do with. Fixing a bottleneck only shows up where that bottleneck
was.

**What it cost.** Eviction became approximate global LRU — each shard trims its own slice, so the
globally-oldest key can survive while a newer one elsewhere is evicted. Whole-keyspace operations
(`KEYS`, `size()`, `snapshot()`) visit shards one at a time rather than freezing the world, so
their totals need not match a single instant. And there is a capacity floor: eviction never takes
a shard below one entry, so a database with N shards holds at least N entries and a memory limit
below that cannot be honoured.

## What the numbers changed

| Earlier claim | Verdict |
|---|---|
| A hand-written hash table is worth building | **Partly.** Slower on inserts and hits; 2.4× faster on misses; carries TTL/LRU metadata inline, which is what made later phases possible. |
| `shared_mutex` beats a plain mutex for cache workloads | **Confirmed** up to 4 threads, **reversed** at 8. |
| The design scales with threads | **Was wrong, now fixed.** The single lock capped it at ~2 threads; sharding took it to 10.7M ops/sec at 8 threads (§7). |
| Returning values by copy is an acceptable cost | **Confirmed for small values** (12%), **expensive for large** (89% at 4 KB). |
| Strict LRU tracking is affordable | **Confirmed** — 3–31%, and not the bottleneck. |
| The memory estimate is "good enough for eviction" | **Over-counts by 1.72×.** Conservative, but should be calibrated. |

## What is next

Sharding is done (§7), and it was the change every measurement pointed at. What is left, in order:

1. **Shrink the node** from 96 bytes toward `std::unordered_map`'s ~64. The TTL field and the two
   recency pointers are what make the custom table lose on lookup hits (§1); packing the
   expiration as a raw integer and dropping the cached footprint would recover roughly 16 bytes.
2. **Calibrate the memory estimate**, which over-counts by 1.72× (§2).
3. **Benchmark against real Redis** on this machine, so the ~200k ops/sec figure has a scale.
4. **An event loop** (kqueue/epoll) to lift the one-worker-per-connection ceiling. Now that the
   lock is no longer the bottleneck, this is what caps concurrent clients.
