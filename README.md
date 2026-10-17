# EmberKV

[![CI](https://github.com/Khalid-tk/emberkv/actions/workflows/ci.yml/badge.svg)](https://github.com/Khalid-tk/emberkv/actions/workflows/ci.yml)

A multi-threaded, Redis-compatible in-memory key-value server written in modern C++20.

EmberKV speaks the Redis wire protocol (RESP2), so stock tools work against it unchanged:
`redis-cli`, `redis-benchmark` and every Redis client library. It supports strings,
counters, lists, TTLs, LRU eviction under a memory limit, and snapshots to disk. It
runs on Linux, macOS and Windows with no dependencies beyond the standard library
(GoogleTest is fetched for the tests).

```text
$ emberkv-server --maxmemory 512mb --snapshot data.ekv
EmberKV 1.0.0 listening on 127.0.0.1:6380
  4 I/O threads, 64 shards, epoll poller, maxmemory 512 MB

$ emberkv-cli
127.0.0.1:6380> SET greeting "hello world" EX 60
OK
127.0.0.1:6380> RPUSH queue job:1 job:2 job:3
(integer) 3
127.0.0.1:6380> LRANGE queue 0 -1
1) "job:1"
2) "job:2"
3) "job:3"
127.0.0.1:6380> INCRBY visits 41
(integer) 41
```

The point of the project is the engineering, not the feature count. Each part below
was chosen to be small enough to read in one sitting and to show a specific idea: an
O(1) LRU with stable iterators, sharded locking with deadlock-free multi-key
operations, a multi-reactor network server, an incremental protocol parser, a
portable socket layer, crash-safe persistence, and a test suite that runs clean under
AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

---

## Contents

- [Building](#building)
- [Running](#running)
- [Commands](#commands)
- [Architecture](#architecture)
- [Design decisions](#design-decisions)
- [Modern C++ used, and where](#modern-c-used-and-where)
- [Testing](#testing)
- [Benchmarks](#benchmarks)
- [Limitations and next steps](#limitations-and-next-steps)
- [Project layout](#project-layout)

## Building

Requirements: CMake 3.20+, a C++20 compiler with `<format>` and `std::jthread`
(GCC 13+, Clang 18+, or Visual Studio 2022),
and Git (CMake uses it to download GoogleTest for the tests).

**Windows (Visual Studio 2022, "Desktop development with C++" workload)**, from a
*Developer PowerShell for VS 2022*:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\emberkv-server.exe
```

You can also open the folder directly in Visual Studio or CLion; both pick up `CMakeLists.txt`.

**Linux / macOS / WSL:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/emberkv-server
```

Build options:

| Option | Default | Effect |
|---|---|---|
| `EMBERKV_BUILD_TESTS` | `ON` | Build the GoogleTest suite (set `OFF` to build offline) |
| `EMBERKV_BUILD_BENCH` | `ON` | Build `emberkv-bench` |
| `EMBERKV_WERROR` | `OFF` | Treat warnings as errors |
| `EMBERKV_SANITIZE` | empty | GCC/Clang sanitizers, e.g. `address,undefined` or `thread` |
| `EMBERKV_FORCE_POLL` | `OFF` | Use the portable `poll()` backend on Linux instead of epoll |

## Running

```text
emberkv-server [options]
  --bind <address>      address to listen on            (default 127.0.0.1)
  -p, --port <port>     TCP port, 0 for any free port    (default 6380)
  --threads <n>         I/O threads                      (default: CPU count)
  --shards <n>          lock shards, rounded up to 2^k   (default 64)
  --maxmemory <size>    memory budget, e.g. 512mb, 2gb   (default 256mb)
  --maxclients <n>      connection limit                 (default 10000)
  --snapshot <file>     load at startup, SAVE/BGSAVE target, saved on shutdown
```

The default port is 6380 so it never collides with a real Redis on 6379. Press
Ctrl+C to stop: the server finishes in-flight work, closes connections and, if
`--snapshot` is set, writes a final snapshot.

Connect with the bundled client (`emberkv-cli`, works on Windows), with `redis-cli -p 6380`,
or with any Redis library, for example in Python: `redis.Redis(port=6380)`.
Plain `telnet`/`nc` works too, since inline commands are supported.

## Commands

| Group | Commands |
|---|---|
| Strings | `GET` `SET key value [EX s \| PX ms \| KEEPTTL] [NX \| XX] [GET]` `GETDEL` `APPEND` `STRLEN` `MSET` `MGET` |
| Counters | `INCR` `DECR` `INCRBY` `DECRBY` (64-bit, overflow-checked) |
| Lists | `LPUSH` `RPUSH` `LPOP [count]` `RPOP [count]` `LLEN` `LRANGE` |
| Keys | `DEL` `EXISTS` `TYPE` `KEYS pattern` `EXPIRE` `PEXPIRE` `TTL` `PTTL` `PERSIST` |
| Server | `PING` `ECHO` `INFO [section]` `DBSIZE` `FLUSHALL` `SAVE` `BGSAVE` `LASTSAVE` `HELP` `QUIT` |
| Compatibility | `COMMAND`, `CONFIG GET`, `CLIENT SETNAME` (what `redis-cli` and `redis-benchmark` probe for) |

Semantics follow Redis, including the edge cases: `WRONGTYPE` errors, `TTL` returning
`-2`/`-1`, `EXPIRE` with a past deadline deleting the key, empty lists ceasing to exist,
and `EXISTS k k` counting a key twice.

## Architecture

```text
                    ┌─────────────────────────── emberkv-server ───────────────────────────┐
 clients ──TCP──▶   │  acceptor thread                                                     │
 (redis-cli,        │      │  accept(), round-robin hand-off                               │
  libraries,        │      ▼                                                               │
  emberkv-cli)      │  reactor 0 … reactor N-1     one thread each, owns its connections,  │
                    │  ┌──────────────────────┐    epoll / poll / WSAPoll                   │
                    │  │ recv → RequestParser │    pipelined requests parsed in a batch     │
                    │  │      → CommandProcessor ─┐                                         │
                    │  │ send ← encode(Reply) │   │                                         │
                    │  └──────────────────────┘   ▼                                         │
                    │                   Store: 64 shards, each { mutex, LruCache }          │
                    │                   one MemoryBudget shared by all shards               │
                    │                   background thread: expire keys, enforce budget      │
                    └──────────────────────────────────────────────────────────────────────┘
```

| Layer | Files | Responsibility |
|---|---|---|
| Storage | `lru_cache.hpp`, `value.hpp`, `store.hpp` | Generic LRU+TTL cache; values as `std::variant`; sharded, thread-safe keyspace |
| Protocol | `resp.hpp` | Incremental RESP request parser, reply encoder, reply parser for clients |
| Commands | `commands.hpp` | Table-driven dispatch, argument validation, Redis semantics |
| Network | `net.hpp`, `server.hpp` | Portable sockets and poller, multi-reactor server |
| Persistence | `snapshot.hpp` | Binary snapshot with checksum and atomic replace |
| Concurrency utilities | `thread_pool.hpp` | Thread pool with futures (used by `BGSAVE`, tests, benchmark) |

## Design decisions

These are the choices worth discussing, and the reasoning behind them.

### 1. An O(1) LRU built on iterator stability

`LruCache` combines three containers: a `std::list` in recency order, an
`std::unordered_map` from key to list node, and a `std::multimap` ordered by expiry
deadline. Promoting an entry is `list::splice`, which moves a node without invalidating
any iterator, so the iterators stored in the two indexes stay valid for the entry's
whole life. Every operation is O(1) on average; TTL bookkeeping is O(log n) in the
number of keys that *have* a TTL.

The index doesn't store a second copy of the key: it holds a pointer to the key inside
the list node, with a transparent hasher so lookups by `std::string_view` work without
allocating. That saves one allocation and one key's worth of memory per entry.

### 2. Two kinds of expiration

Expired keys are removed **lazily** (a read of a dead key deletes it) and **actively**
(a background thread pops the front of the deadline-ordered index). Because that index
is sorted, active expiration touches only dead keys and never scans live ones. Redis
uses random sampling instead; a sorted index costs a little memory per TTL key but makes
cleanup exact.

The clock is injected (`Store::Options::clock`), so TTL tests move time by hand and
never sleep or flake.

### 3. Sharded locks, and why they're plain mutexes

The keyspace is split across 64 shards (configurable), each an `LruCache` behind its own
mutex, so threads working on different keys rarely contend. A few details:

- **`std::mutex`, not `std::shared_mutex`.** In an LRU a read is also a write, since it
  moves the entry to the front. A reader/writer lock would add cost and allow no extra
  concurrency.
- **Fibonacci hashing picks the shard from the *high* bits of the hash.** Each shard's
  hash map buckets by the *low* bits of the same hash (MSVC's uses a power-of-two mask),
  so choosing the shard from the low bits would put every key in a shard into the same
  few buckets.
- **Each shard is `alignas(64)`**, so neighbouring mutexes don't share a cache line
  (false sharing).

### 4. Deadlock-free multi-key commands

`MSET`, `MGET` and `DEL k1 k2 …` lock every shard involved, always in ascending shard
order. Two commands with overlapping keys therefore can't each hold one lock while
waiting for the other's. A dedicated test hammers this with random key orders from six
threads.

### 5. One global memory budget, not a slice per shard

The first version gave each shard `maxmemory / 64` and evicted within the shard. Running
`redis-benchmark` exposed the flaw: its LPUSH test grows one list past 4 MB, which is
1/64 of 256 MB, so the server answered OOM while 98% of memory was free. Now all shards
report to one `MemoryBudget` (an atomic counter). When the total is over the limit, the
shard being written evicts its own least-recently-used entries, and a background pass
trims any remaining excess. Shards never lock each other, and any single key may use the
whole budget. The trade-off is approximate global LRU: the victim is the oldest entry *in
the shard being written*, not in the whole store. Redis approximates LRU too.

Each weight change is one atomic read-modify-write on the shared counter (not a release
followed by a charge), because that counter's cache line is contended by every shard.

### 6. A multi-reactor server with pipelining and backpressure

One acceptor thread hands each new connection to one of N reactor threads, round-robin.
The connection stays on that reactor for its whole life, so per-connection state needs no
locking. A reactor reads whatever is available, parses **every** complete request in the
buffer (pipelining), executes them and writes all replies in one batch.

Two safety valves keep it robust:

- **Fairness.** Each wake-up does a bounded number of reads, so one client streaming
  data can't starve the others on that reactor.
- **Backpressure.** If a client stops reading replies and its pending output passes 64 MB,
  the reactor stops reading from it until it drains. Requests already buffered are run
  once the output is flushed.

### 7. Portability without `#ifdef` spaghetti

Everything platform-specific lives in `src/net.cpp` behind `net.hpp`:

- **`Socket`** is a move-only RAII handle. Exactly one owner closes it, exactly once.
- **`Poller`** uses epoll on Linux, `poll()` on macOS, and `WSAPoll()` on Windows. A pimpl
  keeps `<sys/epoll.h>` and `<winsock2.h>` out of every other file.
- **The wake-up channel** that tells a blocked reactor a new connection arrived is a
  `socketpair` on POSIX and a loopback TCP pair on Windows, because `WSAPoll` can only wait
  on sockets, not pipes.

The rest of the code base doesn't know which OS it is on.

### 8. Snapshots that can't corrupt themselves

A snapshot is written to `file.tmp` and renamed over the old file, so a crash mid-save
leaves the previous snapshot intact. The format is explicitly little-endian, written byte
by byte (no `reinterpret_cast`, so it's portable and free of undefined behaviour), and
ends with an FNV-1a checksum that is verified *before* anything is loaded. A corrupt file
changes nothing. TTLs are stored as wall-clock deadlines, because the store's monotonic
clock means nothing after a restart. `BGSAVE` runs on a worker thread and holds each
shard's lock only while that shard is serialised.

### 9. Error handling

Each failure gets the handling that fits it:

- **Expected command failures** (wrong type, bad integer) are ordinary error replies.
- **Unrecoverable input** (malformed protocol) throws `ProtocolError`. The connection
  replies once and closes, because a RESP stream can't be resynchronised.
- **Unexpected exceptions** inside a command, including `std::bad_alloc`, become an error
  reply instead of killing the server.
- **`LruCache::put` gives the strong exception guarantee.** If any step throws, the three
  containers are rolled back by iterator, and a test injects a failure mid-insert to prove
  it.

## Modern C++ used, and where

| Feature | Where |
|---|---|
| Concepts and `requires` clauses | `Weigher` concept and constrained templates in `lru_cache.hpp`; `Store::with_key`; `ThreadPool::submit` |
| `std::variant` + `std::visit` + `overloaded` | Stored values (`value.hpp`), reply type and encoder (`resp.cpp`), snapshot writer |
| `std::jthread`, `std::stop_token`, `condition_variable_any::wait(…, stop_token, …)` | Reactors, acceptor, expiry sweeper, thread pool: shutdown with no hand-rolled flags |
| `std::span`, `std::string_view`, heterogeneous lookup | Command arguments; allocation-free key lookups via transparent hashing |
| `std::latch` | Benchmark: all worker threads start at the same instant |
| `std::format` | `INFO` output, usage text, error messages |
| `std::bit_ceil`, `std::countr_zero` | Shard count rounding (`store.cpp`) |
| Ranges algorithms | `std::ranges::sort` / `unique` / `equal` |
| `std::from_chars` / `std::to_chars` | Locale-free integer parsing and formatting on the hot path |
| Designated initializers | `Store::Options{.shard_count = 64, .max_memory = …}` |
| Defaulted `operator==` | `resp::Reply` and its alternatives (used heavily by the tests) |
| Pack init-capture (`... args = std::forward<Args>(args)`) | `ThreadPool::submit` moves arguments into the task |
| `[[nodiscard]]`, `[[no_unique_address]]` | Throughout; stateless hashers and weighers take no space |
| Type erasure | `Task`, a move-only `void()` wrapper (concept/model pattern) for the thread pool |
| Rule of five, RAII | `Socket`, `LruCache` (custom moves that hand over budget accounting), `NetworkInit` |
| Pimpl | `net::Poller` hides the OS backend |
| `std::packaged_task` / `std::future` | Thread pool results and exception propagation, `BGSAVE` |
| Relaxed atomics where ordering doesn't matter | Statistics counters, memory budget |
| Thread-safe function-local statics | Command table and lookup index (`commands.cpp`) |

## Testing

111 tests in GoogleTest:

| Suite | What it covers |
|---|---|
| `LruCache`, `LruCacheBudget` | Eviction order, weights, TTL, lazy and active expiry, moves, shared budget accounting, strong exception guarantee via an injected failure |
| `RequestParser`, `RespEncode`, `ReplyParser` | Input split at every byte, pipelining, binary safety, inline commands, malformed input, size limits, round trips |
| `Commands` | Redis semantics for every command, including edge cases and error messages |
| `Glob` (parameterised) | `KEYS` pattern matching |
| `Store`, `StoreConcurrency`, `ThreadPool` | Shard distribution, global memory limit, atomic `INCR` under 8 threads, deadlock freedom, weight accounting under a mixed concurrent load |
| `Snapshot` | Round trip, TTL handling across restarts, corruption detection, truncated and foreign files |
| `ServerTest` | Real sockets: pipelining 4,000 requests in one write, a request sent one byte at a time, 8 MB values, `QUIT`, protocol errors, 16 concurrent clients, client limit, clean shutdown |
| `Config` | Command-line and size parsing |

Every test passes in plain builds, under **AddressSanitizer + UndefinedBehaviorSanitizer**
and under **ThreadSanitizer**, with both the epoll and `poll()` backends. The server
was also run under ThreadSanitizer while `redis-benchmark`, `BGSAVE`, `KEYS` and `INFO`
ran concurrently, with no reports. The code compiles warning-free with
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wold-style-cast` on GCC, on Clang, and
in a Windows cross-build (Clang targeting MinGW).

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DEMBERKV_SANITIZE=thread
cmake --build build-tsan && ctest --test-dir build-tsan
```

`.github/workflows/ci.yml` runs the suite on Linux (GCC and Clang), macOS and Windows
(MSVC), plus the sanitizer and `poll()` builds.

## Benchmarks

All numbers below come from one 2-vCPU cloud VM (Intel Xeon @ 2.1 GHz) with the load
generator on the same machine, so treat them as relative, not absolute.

**Server throughput vs Redis 7.0.15.** `redis-benchmark -n 400000 -c 50`, median of three
runs, requests per second. EmberKV ran with 2 I/O threads; Redis is single-threaded.

| Command | EmberKV | Redis | | EmberKV, pipelined ×16 | Redis, pipelined ×16 |
|---|--:|--:|---|--:|--:|
| SET | 89,766 | 100,326 | | 1,136,363 | 1,173,020 |
| GET | 89,047 | 99,181 | | 1,265,822 | 1,413,427 |
| INCR | 92,980 | 97,656 | | 1,152,737 | 1,324,503 |
| LPUSH | 90,090 | 101,522 | | 1,197,604 | 1,179,941 |
| LPOP | 93,589 | 102,301 | | 1,015,228 | 1,030,927 |
| MSET (10 keys) | 97,205 | 112,076 | | 505,050 | 373,134 |

EmberKV runs at roughly 90% of Redis's throughput unpipelined and is in the same range
pipelined. On two vCPUs shared with the load generator the extra I/O thread has little
room to help; Redis has years of tuning behind it. Profiling `MSET` with Callgrind led to
three changes that cut its instruction count by about 18%: keys are moved instead of
copied, each key is hashed to its shard only once, and keys are stored once instead of
twice. Together with one atomic update per write instead of two (see design decision 5),
they took pipelined `MSET` from about 190,000 to over 500,000 requests per second.

**Lock sharding** (`emberkv-bench`, in-process, 90% GET / 10% SET over 100k keys,
operations per second):

| Threads | 1 shard | 64 shards | Speed-up |
|--:|--:|--:|--:|
| 1 | 1,270,801 | 1,165,913 | 0.92× |
| 2 | 327,886 | 1,412,081 | 4.31× |
| 4 | 812,539 | 1,711,995 | 2.11× |
| 8 | 918,268 | 1,929,493 | 2.10× |

With one thread, sharding costs a little: there is an extra hash, and the data spreads
over 64 maps instead of one. With two threads on two cores, a single lock collapses into
a *lock convoy*: the threads hand the mutex back and forth through the kernel on almost
every operation, and throughput falls fourfold. With 64 shards, contention nearly
disappears. Above two threads the machine is oversubscribed, which happens to blunt the
convoy.

Reproduce with:

```bash
./build/emberkv-bench 2
redis-benchmark -p 6380 -q -n 400000 -c 50 -t set,get,incr,lpush,lpop,mset
```

## Limitations and next steps

- **No replication, append-only log, authentication or TLS.** Snapshots are the only
  persistence.
- **`KEYS` is O(n)**, as in Redis. A cursor-based `SCAN` would be the next command to add.
- **Snapshot memory.** A snapshot is serialised in memory before being written, which
  briefly doubles the memory it uses. Streaming it shard by shard would fix that.
- **Approximate global LRU.** Eviction picks the oldest entry in the shard being
  written, not in the whole store.
- **Allocation on the hot path.** The parser allocates one `std::string` per argument.
  A per-connection arena or small-buffer argument type is the next optimisation the
  profile points to.
- **More data types.** Hashes and sets would mean one more `std::variant` alternative
  each. Every non-exhaustive `std::visit` would then fail to compile until it handles
  them, which is the point of using a variant.

## Project layout

```text
include/emberkv/   public headers: lru_cache, value, store, resp, commands,
                   net, server, snapshot, thread_pool, config
src/               implementation and main.cpp (emberkv-server)
tools/cli.cpp      emberkv-cli, an interactive client
tests/             GoogleTest suite (unit, concurrency, integration)
bench/             sharding benchmark
.github/workflows  CI: Linux, macOS, Windows, sanitizers
```

## License

MIT. See [LICENSE](LICENSE).
