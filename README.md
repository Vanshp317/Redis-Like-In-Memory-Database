# VCache

A Redis-inspired, single-node, in-memory key-value database written from scratch in C++17.

Clients connect over TCP and speak a simple text protocol. The project is built bottom-up in
phases, each one independently testable.

It was built to a written specification (a "discovery document") that is not included in this
repository. Section references throughout these notes -- §16 on benchmarking, §19 on the CLI, and
so on -- point at that document.

## Status

| Phase | Component | State |
|-------|-----------|-------|
| 1 | Custom hash table | **Done** |
| 2 | `Database` API (`set`/`get`/`del`/`exists`/`keys`) | **Done** |
| 3 | Command parser | **Done** |
| 4 | TCP server | **Done** |
| 5 | Thread pool / concurrency | **Done** |
| 6 | TTL expiration | **Done** |
| 7 | Persistence (snapshot + restore) | **Done** |
| 8 | LRU eviction | **Done** |
| 9 | Benchmarking | **Done** |

## Running it

```sh
./vcache-server --port 6379
```

Then talk to it with the bundled client:

```
$ vcache-cli
Connected to 127.0.0.1:6379
Type 'help' for commands, 'exit' to quit.
vcache> SET username Vansh
OK
vcache> GET username
Vansh
vcache> SET session abc123 EX 30
OK
vcache> SAVE
OK
vcache> KEYS
(2 keys)
1) "session"
2) "username"
```

`vcache-cli` also runs one-shot and from a pipe, so it scripts:

```sh
vcache-cli GET username              # prints the value; exit 1 if the server errors
vcache-cli SET greeting "hello world"
echo "KEYS" | vcache-cli
vcache-cli --host db.internal --port 6380 GET session
```

There is no line-editing or history built in — that would mean a readline dependency, and the
project has none. `rlwrap vcache-cli` gives you both for free.

Or use any line-oriented TCP client:

```
$ nc 127.0.0.1 6379
SET username Vansh
OK
GET username
Vansh
SET greeting "hello world"
OK
SET session abc123 EX 2
OK
KEYS
(3 keys)
1) "greeting"
2) "session"
3) "username"
GET session          # ...two seconds later
(nil)
DEL username
1
GET username
(nil)
```

`--bind <addr>` changes the listen address (default `127.0.0.1`) and `--threads <n>` the worker
count (default 64). Ctrl+C shuts down cleanly, even with clients still connected. Networking is
POSIX-only (macOS/Linux); there is no Winsock path.

To keep data across restarts:

```sh
./vcache-server --dbfile ./vcache.snapshot --save-interval 30
```

`--dbfile` enables persistence; without it, nothing is ever written and `SAVE` reports that it is
not configured. `--save-interval` adds automatic snapshots on top of manual `SAVE`. A clean
shutdown always takes a final snapshot, so a deliberate restart loses nothing.

To cap memory and evict the least recently used keys past it:

```sh
./vcache-server --max-memory 100MB
```

Suffixes are 1024-based (`100MB`, `512kb`, `1G`); a bare number is bytes. Without the flag there
is no limit and nothing is ever evicted.

## Building

Requires CMake 3.16+ and a C++17 compiler. Verified against CMake 4.4.2 and Apple clang 17
on macOS; the build is POSIX-only.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Build options:

| Option | Default | Effect |
|--------|---------|--------|
| `VCACHE_BUILD_TESTS` | `ON` | Build the test suite |
| `VCACHE_BUILD_BENCHMARKS` | `ON` | Build the benchmarks (not run by CTest) |
| `VCACHE_ENABLE_ASAN` | `OFF` | AddressSanitizer + UBSan (memory errors) |
| `VCACHE_ENABLE_TSAN` | `OFF` | ThreadSanitizer (data races) |

ASan and TSan are mutually exclusive — use separate build directories. Neither belongs in a
benchmark build.

```sh
cmake -S . -B build-asan -DVCACHE_ENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DVCACHE_ENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
```

The build defaults to `Release`, since benchmark numbers from a debug build are meaningless.

### Without CMake

There are no third-party dependencies, so a compiler alone is enough:

```sh
mkdir -p build/manual
clang++ -std=c++17 -O2 -Wall -Wextra -Iinclude src/*.cpp server/main.cpp \
    -o build/manual/vcache-server
clang++ -std=c++17 -O2 -Wall -Wextra -Iinclude src/*.cpp client/main.cpp \
    -o build/manual/vcache-cli

for suite in test_hashtable test_database test_command_parser test_command_executor \
             test_thread_pool test_expiration test_persistence test_eviction \
             test_concurrency test_server test_client; do
    clang++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itests \
        src/*.cpp "tests/$suite.cpp" -o "build/manual/$suite" && "./build/manual/$suite"
done
```

## Layout

```
include/        public headers
  Entry.h       one stored record: key, value, optional expiration
  HashTable.h   custom separate-chaining hash table
  Database.h    the keyspace and the policy layer over it
  CommandParser.h    text protocol -> structured Command
  Protocol.h         the type-tagged reply format, shared by both ends
  CommandExecutor.h  Command -> Database call -> tagged reply
  ThreadPool.h       fixed worker threads over a shared task queue
  ExpirationSweeper.h  background reclamation of expired keys
  Persistence.h        snapshot format + atomic save/load
  SnapshotScheduler.h  periodic snapshots
  Server.h           TCP sockets, framing, connection lifetime
  VCacheClient.h     client-side connection and reply assembly
src/            implementations
server/         the vcache-server daemon
client/         the vcache-cli interactive client
tests/          test suites + a small dependency-free test framework
bench/          benchmarks (see BENCHMARKS.md)
```

Layering is strict: the server and command parser will talk to `Database` only, and
`Database` is the sole owner of the `HashTable`. TTL, persistence, eviction and locking all
attach at the `Database` level, so none of them leak into the storage container or the
network code.

## Design notes

**The hash table is hand-written on purpose.** It is the core artifact of the project, and in
Phase 9 it becomes the subject of a benchmark against `std::unordered_map`.

- **Collision handling:** separate chaining with singly linked lists. New keys are pushed to the
  front of the chain (O(1), no tail pointer needed).
- **Bucket count is always a power of two,** so the bucket index is `hash & (n - 1)` rather than a
  modulo — masking is far cheaper than 64-bit division on the lookup path.
- **Hash:** FNV-1a followed by a splitmix64 avalanche step. FNV-1a alone has weak low bits, and
  masking only looks at low bits. FNV is used instead of `std::hash` because `std::hash` for
  strings may be randomized per process, which would make benchmarks irreproducible.
- **Growth:** doubles when the load factor would exceed 0.75. Rehashing relinks existing nodes
  rather than reallocating them, so no key or value bytes are copied.
- **Ownership:** each chain is a `std::unique_ptr` list. Chains are torn down iteratively, not
  recursively — the default recursive destructor would use one stack frame per element and could
  overflow the stack on a long chain.
- **`HashTable` is not thread-safe.** Locking arrives one layer up in Phase 5, so single-threaded
  callers pay nothing for it.

**`Database::get()` returns `std::optional<std::string>` by value, not a pointer.** A pointer into
the table would dangle the instant the Phase 5 lock is released and another client overwrote or
evicted the key. Copying on read makes the signature survive concurrency unchanged. Measured cost:
12% of throughput at 16-byte values, 89% at 4 KB (see `BENCHMARKS.md`). For large-value workloads
the fix is `shared_ptr<const string>` values, not raw pointers.

**Validation is not the storage layer's job.** `Database` accepts empty keys and arbitrary bytes.
Rejecting *malformed commands* — wrong arity, unknown verb, bad TTL — is the command parser's
job, so there is exactly one place to look when a client gets an error.

## The wire protocol

One command per line. Tokens are whitespace-separated; trailing `CR`/`LF` is stripped, so telnet
and netcat work by hand.

```
SET key value [EX seconds]   GET key   DEL key   EXISTS key   KEYS   SAVE
```

Verbs are case-insensitive. **Keys and values are not** — folding those would silently merge
distinct keys, which is data loss rather than convenience.

Arguments may be double-quoted to carry spaces, with `\n`, `\r`, `\t`, `\b`, `\a`, `\\`, `\"` and
`\xHH` escapes inside:

```
SET greeting "hello world"
SET blob "\x00\xff binary is fine"
```

Quoting exists because storage is binary-safe; a protocol that could not express a value
containing a space would throw that away. `\xHH` lets a client send arbitrary bytes over a text
connection.

**Replies are type-tagged**, so a reply says what it is without the reader needing to know what
was asked:

| Tag | Meaning | Example |
|---|---|---|
| `+` | status | `+OK` |
| `-` | error | `-ERR unknown command 'FOO'` |
| `$` | value | `$Vansh` |
| `_` | nil | `_` |
| `:` | integer | `:1` |
| `*` | array header | `*2`, then that many tagged lines |

This replaced an untagged format, and it was writing the client that forced the change. Two bugs
of the same shape appeared, and neither was fixable by escaping harder — the problem was that
values and control messages shared one namespace:

- `SET k "(2 keys)"` then `GET k` returned a line byte-for-byte identical to a two-element `KEYS`
  header. A client reading the reply alone waited forever for two lines that were never sent.
- `SET k "ERR disk full"` then `GET k` returned a line that a client checking for an `ERR ` prefix
  reported as a server failure. A stored value was mistaken for an error.

Both are now impossible: `$(2 keys)` and `$ERR disk full` are unambiguously values. The client
carries a regression test for each.

The cost is that a raw `nc` session shows the tags. That is a deliberate trade — the **wire** is
machine-first, and `vcache-cli` renders it back into the human form from §19. Redis makes exactly
this trade, for exactly this reason. It also makes the format extensible: a new reply type is a
new tag, and an old client reports an unrecognised tag rather than silently misreading it.

Values are still escaped on the way out with the same scheme the parser accepts on the way in, so
a value can never contain a newline and break the one-reply-per-line framing.

Parser design notes:

- **Stateless and static.** No per-connection state, so Phase 5 can call it from every worker
  thread with no locking.
- **No exceptions on the request path.** Every failure is a returned `ParseResult`. That is the
  parser's half of the §17 requirement that bad input can never take the server down.
- **Adding a verb is one table row** in `kCommands` plus one enum value. Phase 6 widens the SET
  row from `{2, 2}` to `{2, 4}` for `SET key value EX seconds`; until then that form is rejected,
  because accepting a TTL nothing honours is worse than refusing it.
- **Echoed client bytes are truncated and sanitized.** An unknown verb is reflected back in the
  error, so it is capped at 32 characters with non-printables replaced — a 10 KB typo must not
  become an amplification, and control bytes must not forge extra lines in a line-oriented
  protocol.
- **Locale-independent case folding.** ASCII-only, not `std::toupper`: under a Turkish locale
  `toupper('i')` is `İ`, and the server would stop recognising `exists`.

## Server notes

**`CommandExecutor` is separate from `Server` on purpose.** Everything about what a request
*means* is a pure string-in/string-out function, testable without opening a socket. `Server` is
then a thin shell that only moves bytes, and the parts that are awkward to test are the parts with
the least logic in them.

- **TCP is a byte stream, not a message stream.** One client write can arrive as three reads, and
  three writes can arrive as one. Bytes accumulate in a per-connection buffer and commands are cut
  out at newlines. Assuming one read equals one command is the classic socket bug: it works on
  localhost and falls apart over a real network. Both directions are tested — a command split
  across three packets, and three commands pipelined into one packet.
- **`send()` may accept fewer bytes than offered,** so it drives a loop. Treating it as
  all-or-nothing silently truncates large replies.
- **SIGPIPE is disabled per socket** (`SO_NOSIGPIPE` on macOS/BSD, `MSG_NOSIGNAL` on Linux). Its
  default action is to kill the process, so without this a client pressing Ctrl+C could take the
  database down with it.
- **`SO_REUSEADDR`,** so restarting doesn't fail with "Address already in use" while the old
  socket sits in `TIME_WAIT`.
- **Shutdown uses a self-pipe.** `run()` waits on the listening socket *and* a pipe via `poll()`;
  `stop()` writes one byte to the pipe. Closing the listening socket from another thread while
  `run()` is blocked in `accept()` races against file-descriptor reuse. `stop()` also
  `shutdown()`s an idle client socket, so Ctrl+C works even with a connected but silent client.
- **A request-size cap** (`maxRequestBytes`, default 8 MiB) bounds the per-connection buffer. The
  parser cannot enforce this because it only ever sees complete lines; a client that never sends a
  newline is the case being defended against.
- **Loopback by default.** VCache has no authentication yet, and an unauthenticated database
  listening on `0.0.0.0` is how servers get wiped. Binding wider is a deliberate `--bind` choice.
## Concurrency

**One `std::shared_mutex` in `Database`.** Reads (`get`, `exists`, `keys`, `size`) take it shared
and run concurrently; writes (`set`, `del`, `clear`) take it exclusively. A reader/writer lock
rather than a plain mutex because cache workloads are overwhelmingly reads. It isn't free —
`shared_mutex` costs more per acquisition than a plain mutex and can lose on write-heavy
workloads. Phase 9 should measure plain mutex vs. this vs. per-shard locks rather than assume.

**The lock lives in `Database`, not `HashTable`.** Locking the container would mean each
operation takes the lock separately, so a read-modify-write spanning two calls would still race —
and single-threaded users would pay for a lock they don't need. `HashTable` stays deliberately
unsynchronised.

**`Database::get()` returning a copy is what makes this work.** The copy happens while the lock
is held. That decision was made in Phase 2 for exactly this moment, and no signature had to change
to become thread-safe.

**One worker per connection, for the connection's lifetime.** So `--threads` is also the ceiling
on simultaneous clients. It's sized by expected connections, not core count, because workers spend
nearly all their time blocked in `recv()` rather than using CPU. Lifting the ceiling properly
means the kqueue/epoll event-loop rewrite listed under future features, where one thread can watch
thousands of idle sockets.

**A bounded queue, and an honest refusal.** Past `maxQueuedConnections`, new clients get
`ERR server is busy` and are disconnected rather than left holding a connection nothing will
answer. An unbounded queue is a memory leak with extra steps under sustained overload.

**Task exceptions are caught and counted.** An exception escaping a thread's entry function calls
`std::terminate`. One bad task must not kill the server.

**Shutdown.** `stop()` is async-signal-safe — it only stores an atomic and writes one byte to the
self-pipe, so a `SIGINT` handler can call it. Everything needing a mutex happens back on `run()`'s
thread, because a signal handler must never take a lock the interrupted thread might already hold.
`run()` then `shutdown()`s every live connection so workers parked in `recv()` wake up, and joins
the pool. Descriptors are only ever closed while holding `connectionsMutex_`, which is what stops
a descriptor from being closed and reused between being read out of the set and being shut down.

**Verifying it.** A passing concurrency test proves little on its own — races hide for thousands
of runs. The suites run under ThreadSanitizer, and the harness itself was validated by deleting
the locks from `Database.cpp` and confirming TSan immediately reports races in `HashTable::put`
and `HashTable::resize`.

## Expiration

`SET key value EX <seconds>` gives a key a lifetime. Expiry is split in two, and the split is the
central design decision of the phase:

- **Reads hide expired keys.** `GET`, `EXISTS` and `KEYS` treat a lapsed entry as absent the
  instant it expires, but never delete it.
- **`removeExpired()` reclaims them,** driven by `ExpirationSweeper` on a timer.

Redis deletes the key during the read that finds it expired, but Redis is single-threaded so that
costs it nothing. Here a read holds the *shared* lock, and deleting under a shared lock isn't
allowed. Making reads exclusive would serialise every `GET` in the server to handle already-dead
data; upgrading the lock mid-read means dropping it, re-acquiring exclusively and re-checking,
which adds a race window to the hottest path. So reads stay genuinely read-only. The cost is that
an expired key occupies memory until the next sweep reaches its bucket — never visible to a client
in the meantime.

**Sweeping is incremental.** Each pass scans a bounded slice of buckets (default 128) and resumes
where it left off. Sweeping the whole table would hold the exclusive lock for the length of the
scan, stalling every client on a large keyspace. A resize can leave the cursor out of range; it
just wraps, since a rehash only moves entries between buckets and anything skipped is caught next
pass.

**`steady_clock`, not `system_clock`** — a 60-second TTL must mean 60 elapsed seconds even if NTP
or an administrator moves the wall clock. The cost is that these timestamps are measured from an
arbitrary boot-relative origin and are meaningless across a restart — see the persistence section
for how snapshots handle that.

**`EX 0` and negative TTLs are rejected**, not silently treated as permanent or already-expired —
both would quietly do something the client didn't ask for. TTLs are capped at 100 years, because
adding an unbounded number of seconds to a `time_point` is signed overflow, i.e. undefined
behaviour. Rejecting absurd values in the parser means the executor never has to re-check.

**There is no `TTL` command.** The doc's command set is SET/GET/DEL/EXISTS/KEYS, and
`Database::ttl()` exists for persistence and tests rather than the protocol. Exposing it would be
one row in the command table if you want it.

**`size()` still counts unreclaimed expired keys**, like Redis `DBSIZE`. Filtering would make a
cheap accessor O(n); use `keys().size()` for the live count.

## Persistence

A snapshot of the whole keyspace, written by `SAVE`, by `--save-interval`, and on clean shutdown;
read back at startup.

**The format is length-prefixed binary, not text.** Keys and values may contain NULs, newlines,
any byte at all, so a text format would need an escaping layer with its own bugs and a parser to
undo it. Length prefixes make the question disappear: the length says how many bytes to read and
no byte is special. The doc suggests starting with text and moving to binary later; skipping the
text step avoids building an escaping scheme only to delete it.

```
header   "VCACHE" (6)  formatVersion (u16)  entryCount (u64)
entry    keyLength (u32)  key  valueLength (u32)  value  expiresAtMillis (i64)
footer   checksum (u64, FNV-1a over everything above)
```

Little-endian throughout, so a snapshot written on one machine reads on another.

**Expiry is stored as an absolute wall-clock time, not a remaining duration.** A `steady_clock`
value is measured from an arbitrary boot-relative origin and means nothing in a file. Storing the
remaining duration would be actively wrong: a server down for an hour would resurrect every
60-second key with a fresh 60 seconds, and such a key could never die. An absolute time means keys
that lapsed while the process was gone are dropped on load. The clock conversion happens entirely
inside `Persistence.cpp`.

**Writes are atomic.** The snapshot goes to `<path>.tmp`, is `fsync`ed, then `rename`d over the
target — `rename` is atomic on POSIX, so a crash leaves either the complete old snapshot or the
complete new one. Writing in place means a crash mid-write destroys the only copy. Without the
`fsync`, the rename could publish a file whose bytes have not reached the disk yet. (A fully
crash-proof rename would also `fsync` the containing directory; that gap is not closed here.)

**A corrupt snapshot stops the server from starting.** Starting with an empty keyspace would look
like success, and then the first scheduled snapshot would overwrite the damaged file with an empty
one — turning recoverable corruption into permanent data loss. A *missing* file is different and
is treated as a normal first run.

**The decoder treats the file as untrusted input.** The checksum is verified before any length
field is acted on; lengths are bounded at 512 MiB and checked against the remaining bytes; the
entry-count reservation is capped; trailing bytes are rejected. A failed load leaves the database
exactly as it was rather than applying a partial restore.

**`snapshot()` copies the keyspace under the read lock**, which briefly doubles memory. The
alternative — writing to the file while holding the lock — would block every writer for the length
of a disk write. Redis avoids both by forking and letting copy-on-write do the work, which is the
future-work path.

**A failed scheduled save is counted, not fatal.** A full disk must not take the server down; the
error is recorded and the next interval retries. A failed `SAVE` command, by contrast, is reported
to the client — someone who asked for durability needs to know they did not get it.

## Benchmarks

Measured results, methodology, and the claims they overturned are in **[BENCHMARKS.md](BENCHMARKS.md)**.
Headlines from an M3 Pro:

- **~200,000 ops/sec** over TCP, saturating at ~10 concurrent clients; GET p99 under 600 µs at
  100 clients.
- **652,000 ops/sec** pipelined from one client — the 10× gap over un-pipelined is network round
  trip, not database work.
- The custom hash table is **0.69×** `std::unordered_map` on lookup hits and **2.36×** on misses.
- Throughput peaks at **two threads**. The single global lock is the ceiling; sharding it is the
  highest-value next change.

```sh
cmake -S . -B build && cmake --build build -j
./build/bench/bench_hashtable && ./build/bench/bench_database && ./build/bench/bench_server
```

Benchmarks are deliberately not registered with CTest: they take minutes, and a timing result is
not a pass/fail.

## Eviction

`--max-memory` sets a ceiling. Past it, writes evict the least recently used entries until usage
is back under. Without the flag there is no limit and no eviction, and the recency bookkeeping is
skipped entirely.

**The recency list is intrusive.** A doubly linked list threads through the existing hash-table
nodes, most-recently-used first, so eviction is O(1): the victim is always the tail, and promoting
a key is a handful of pointer writes. The Discovery Document's suggested layout has a separate
`LRUCache` class, but a standalone structure would need its own hash map and every operation would
cost two lookups instead of one.

**A GET is a write, and that is the hard part.** Promoting a key mutates the recency list, so a
read is no longer purely a read — which collides with the reader/writer split relied on since
Phase 5. The compromise: the table lock stays *shared* for the lookup and the value copy (the
expensive parts), and only the pointer surgery takes a second, much narrower mutex inside
`HashTable`. Making GET exclusive instead would serialise every read in the server.

That narrow mutex is still a point every GET touches, which is exactly why Redis uses *sampled
approximate* LRU rather than a strict list. **Measured cost: 9% single-threaded, 31% at two
threads, ~3% beyond** — cheaper than feared, and not what limits this server.

**`EXISTS` and `KEYS` do not count as a use.** They are introspection, not access — a monitoring
loop calling `EXISTS` over the keyspace would otherwise flatten the recency order and make
eviction arbitrary.

**Memory accounting is an estimate, deliberately** — and measured to over-count by **1.72×**
against real RSS, so a `--max-memory 100MB` starts evicting at roughly 58 MB of true footprint.
Conservative rather than dangerous, but it should be calibrated. `sizeof(std::string)` is 24 bytes whether it
points at 3 bytes or 3 megabytes, so the footprint is computed as `sizeof(Node)` plus the key and
value lengths plus a flat allowance for allocator overhead. It ignores size-class rounding, treats
short strings as if they allocated when the small-string optimisation means they did not, and
cannot see fragmentation. It is a proxy for memory pressure good enough to drive eviction — not a
figure to quote as the process's real footprint. The tests assert on *ordering* and on
*consistency* (memory returns to baseline after removals), never on an absolute byte count.

**An entry larger than the whole limit is rejected**, not stored and then immediately evicted.
Storing it would report `OK` while losing the data, and the client could not tell until a later
`GET` returned `(nil)`. Eviction also stops at one remaining entry: the bucket array counts toward
usage and never shrinks, so a limit below it would otherwise spin evicting nothing.

**Recency is not persisted.** A snapshot has no recency field, so after a restore the file's order
stands in for it. A restored database that immediately hits its limit evicts in file order rather
than true historical order.
