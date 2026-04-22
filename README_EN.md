# minisqld — Multi-threaded Mini DBMS in Pure C

> A single-process HTTP DBMS daemon written in C11 with **zero external
> runtime dependencies** (only `pthread`). Built on top of a W7 B+Tree
> index engine, exposed over an HTTP/1.1 API server implemented from
> scratch.

The project is a learning exercise run as a one-day hackathon, extended
over two refactoring rounds. Round 1 put the engine behind an HTTP server
with a fixed thread pool and per-table RW locks. Round 2 adds a
self-tuning thread pool, a result cache, and a trie-based prefix index
for autocomplete.

---

## Project Status

**Round 2 refactoring — in progress.** Round 1 has been merged to `dev`
(4 feature PRs integrated). Round 2 feature work is being implemented on
per-owner branches; the scope is frozen and listed under
[Team](#team).

| Round | Scope | Status |
|---|---|---|
| Round 1 (MP0~MP4) | Daemon skeleton, HTTP/1.1 server, fixed thread pool, per-table RW locks, stub-driven mix-merge | ✅ merged to `dev` |
| Round 2 | Dynamic thread pool, LRU cache, Trie prefix search, redesigned UI | ⏳ in progress |
| Final | Integration, benchmarks, README numbers, `dev → main` | ⏳ pending |

---

## Features

**Round 1 (merged):**
- Pure C11 — no HTTP / JSON libraries. `pthread` is the only runtime
  dependency.
- HTTP/1.1 server implemented from scratch (`src/server.c`,
  `src/protocol.c`, `src/router.c`). Static-file serving included.
- Fixed worker thread pool with blocking job queue
  (`src/threadpool.c`).
- Per-table reader/writer locks + global catalog lock
  (`src/engine_lock.c`). Reads (SELECT) run concurrently; writes
  (INSERT/UPDATE/DELETE) serialize per table.
- `?mode=single` toggle for a global-mutex baseline, useful for A/B
  comparison against the multi-worker path.
- W7 B+Tree index (`src/bptree.c`, `src/index_registry.c`) reused
  in-process — no per-request subprocess spawn.
- ANSI REPL client (`client/repl.c`) for terminal-based use and demo
  backup.

**Round 2 (in progress):**
- **Dynamic thread pool.** Starts at 4 workers, scales up by +4 when
  utilization reaches 80%, capped at 16. Shrink policy is deliberately
  left unimplemented in this round (see
  [Known issues](#known-issues--future-work)).
- **LRU result cache with reader/writer lock.** Concurrent readers share
  the cache; writers (INSERT) invalidate affected keys.
- **Trie-based prefix search.** Enables autocomplete queries in
  `O(k)` over the input prefix length, in addition to the existing
  B+Tree point/range lookups.
- **Redesigned frontend.** Minimal Apple/Toss-inspired UI replacing the
  dark-theme stress page used during Round 1.

---

## Architecture

```
┌─────────────┐
│ Client (FE) │  Apple/Toss-inspired UI
└──────┬──────┘
       │ HTTP/1.1 (TCP, Connection: close)
       ▼
┌──────────────────────────────────────┐
│ Thread Pool (dynamic, 4 → 16)        │
│  trigger: utilization >= 80%, +4     │
└──────┬───────────────────────────────┘
       ▼
┌───────────────────────────────┐
│ Query Parser → Planner        │
└──────┬────────────────────────┘
       ▼
┌───────────────────────────────┐       ┌─────────────────────────┐
│ Cache Layer  (LRU + RWLock)   │◀──────│ invalidate on INSERT    │
└──────┬────────────────────────┘       └──────────┬──────────────┘
       │ miss                                      │
       ▼                                           │
┌───────────────────────────────┐                  │
│ Trie Index  +  B+Tree Storage │──────────────────┘
└───────────────────────────────┘
```

| Layer | File | Concurrency |
|---|---|---|
| Socket | `src/server.c` | `accept()` on main thread; `fd` handed to pool |
| Thread pool | `src/threadpool.c` | mutex + condvar queue; dynamic resize (R2) |
| HTTP | `src/protocol.c`, `src/router.c` | stateless per worker |
| Cache | `src/cache.c` (R2) | LRU, reader/writer lock |
| Engine | `src/engine.c`, `src/engine_lock.c` | per-table RW lock + catalog lock + single-mode mutex |
| Index | `src/bptree.c`, `src/trie.c` (R2) | read-side locked via engine layer |
| Storage | `src/storage.c` (W7) | engine layer serializes writes |

Diagram mirrors `CLAUDE.md § Architecture`. The module-boundary contracts
for Round 2 are defined in `CLAUDE.md § 모듈 간 인터페이스` and are still
flagged `TBD` until the four teams confirm signatures.

---

## Demo Scenario

**Use case: a language-dictionary service.** Chosen because it exercises
all three Round 2 features on the same data set:

1. **Concurrent reads (SELECT).** Many users simultaneously look up
   definitions by word ID or exact spelling. Exercises per-table
   read-lock sharing and cache hits.
2. **Operator insert (INSERT).** An admin adds a new word. Exercises
   per-table write-lock serialization and cache invalidation of stale
   prefix / exact-match results.
3. **Real-time autocomplete.** As the user types, the UI hits a
   prefix-search endpoint. Exercises the trie index and concurrent read
   throughput.

The demo page (`web/concurrency.html`) drives these three flows and
surfaces the metrics that come out of `GET /api/stats` — active workers,
queue depth, and accumulated lock wait.

Caveat: browsers cap per-origin HTTP/1.1 connections at 6. The built-in
stress tab cannot fully express the server's multi-worker throughput on
its own. Use `xargs -P` or the REPL for the real number — see
[Benchmark](#benchmark).

---

## Quick Start

```bash
git clone https://github.com/JYPark-Code/jungle_w8_minisql_server.git
cd jungle_w8_minisql_server

# Build (no extra dependencies beyond a C11 toolchain and pthread)
make

# Create a data directory and launch the daemon
mkdir -p data
./minisqld --port 8080 --workers 8 --data-dir ./data --web-root ./web
```

Once stderr reports `[server] listening on port 8080`, open a browser at
<http://localhost:8080/> or hit the API directly:

```bash
curl -X POST http://localhost:8080/api/query \
     -H "Content-Type: text/plain" \
     -d "SELECT * FROM users WHERE id = 1"
```

### Requirements
- GCC 9+ (or Clang with equivalent C11 support)
- GNU Make
- `pthread` (part of glibc on Linux)
- `valgrind` (optional, for `make valgrind`)

No additional libraries, language runtimes, or package managers are
involved at build time or runtime. The daemon produces a single
statically-linkable binary.

### CLI Options

| Option | Default | Description |
|---|---|---|
| `--port N` | `8080` | Listen port |
| `--workers N` | `8` | Initial worker count. Round 2 adds dynamic scaling up to 16. |
| `--data-dir PATH` | `./data` | CSV + schema storage root |
| `--web-root PATH` | `./web` | Static-file root for the frontend |
| `--help` | — | Usage |
| `--version` | — | Version string |

---

## API

All responses are `Content-Type: application/json`.

| Method | Path | Description |
|---|---|---|
| `POST` | `/api/query` | Execute one or more SQL statements (body: raw SQL, `text/plain`) |
| `POST` | `/api/query?mode=single` | Same as above but forced through the global mutex; used as the serial baseline |
| `GET` | `/api/explain?sql=...` | Reports index usage and node-visit count for the statement |
| `GET` | `/api/stats` | Running counters (total queries, cumulative lock wait, active workers) |
| `POST` | `/api/inject` | Bulk-inject dummy rows. **Placeholder in Round 1** (returns 501); wired up during Round 2. |
| `GET` | `/` and `/<file>` | Static files under `--web-root` |

Round 2 will add:

| Method | Path | Description |
|---|---|---|
| `GET` | `/search?q=...` | Exact match on the indexed key column |
| `GET` | `/autocomplete?prefix=...` | Trie prefix query (length-capped) |
| `POST` | `/admin/insert` | Admin write path with cache invalidation |

The Round 2 endpoints are tracked as `TBD` contracts in `CLAUDE.md`
until implementation merges.

---

## Build Targets

```bash
make                # Build ./minisqld
make test           # Run unit + regression tests (W7 + W8 modules)
make tsan           # ThreadSanitizer build → ./minisqld_tsan
make valgrind       # Re-run tests under valgrind --leak-check=full
make bench          # B+Tree microbenchmark from W7 (reference)
make repl           # ./minisqld-repl, an ANSI HTTP client for the daemon
make clean
```

`CFLAGS` used by the default build:
`-Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g`.
CI enforces `-Werror`.

---

## Benchmark

> The numbers below will be filled in after the presentation. The table
> shape is fixed so that runs can be compared round-to-round.

Methodology (planned): 10K random `SELECT` on a 100K-row dictionary
table, warm caches excluded for the "Single-thread" and "Multi-thread"
columns, included for "+Cache". Measured via `xargs -P N` against the
daemon to bypass browser connection limits.

| Scenario | Single-thread | Multi-thread | +Cache | +Dynamic Pool |
|---|---|---|---|---|
| 1K SELECT | TBD | TBD | TBD | TBD |
| 10K SELECT | TBD | TBD | TBD | TBD |

**Round 1 snapshot** (empty `users` table, 64 concurrent requests via
`xargs -P 8`, measured directly against the daemon):

| Scenario | Total elapsed |
|---|---|
| Serial (`xargs -P 1`) | ~380 ms |
| Multi (`xargs -P 8`, `/api/query`) | ~85 ms |
| Single (`xargs -P 8`, `?mode=single`) | ~145 ms |

This is a workload-light snapshot (empty table → server work is
dominated by HTTP framing). Round 2 will re-run the numbers on a 100K
dictionary. For the pure B+Tree reference (up to 1,842× speedup on
indexed reads vs linear scan), see
[`docs/README_w7.md`](docs/README_w7.md).

---

## Team

- **지용 (Lead)** — Dynamic thread pool, graceful shutdown
- **동현** — LRU cache, reader-writer lock
- **승진** — Frontend (Apple/Toss-inspired), autocomplete UI
- **용** — Trie data structure, prefix search query support

Round 1 ownership was organized differently (engine / server / threadpool
primitives / PM infra). Round 2 owners are listed above.

---

## Known Issues / Future Work

- **Thread pool shrink policy is unimplemented in Round 2.** Scale-up at
  80% utilization is in scope; scale-down (idle worker release) is
  pending. Long-lived daemons will keep peak-size pools until restart.
- **Cache coherency model.** Round 2 uses *invalidate-on-write*: an
  `INSERT` drops affected cache entries. A write-through variant is out
  of scope for this round.
- **HTTP Keep-alive unsupported.** Each request uses its own TCP
  connection (`Connection: close`). Browser-based stress tests are
  therefore capped at the per-origin connection limit (6). Use
  `xargs -P` or the REPL for load numbers.
- **`/api/inject` finalized in Round 2.** The Round 1 router returns
  `501 Not Implemented` for this endpoint.
- **CI toolchain install overhead.** `apt-get update` runs on every
  GitHub Actions job (build / test / tsan / valgrind). A follow-up
  (`chore/ci-speedup`) will remove it from jobs that don't need
  `valgrind`.

---

## Directory Structure

```
.
├── include/               Public interface headers (PM-managed)
├── src/                   C sources (W7 engine + W8 layers)
├── tests/                 Regression + concurrency unit tests
├── bench/                 B+Tree pure benchmark (W7)
├── scripts/               Fixture generators and load scripts
├── client/                ANSI REPL HTTP client
├── web/                   Frontend (concurrency demo)
├── docs/                  Architecture diagrams, prior-round README
├── agent/                 Archived AI-collaboration context (prior rounds)
├── prev_project/          Archived prior-round web assets
│   ├── sql_parser_demo/   W6 parser demo
│   └── bp_tree_demo/      W7 B+Tree demo
├── .github/workflows/     CI (build / test / tsan / valgrind)
├── CLAUDE.md              Project-wide design and decision log
├── agent.md               Round 1 design decisions and milestones
├── TEAM_RULES.md          Branching, PR, and CI rules
├── w8_handoff.md          Round 1 → Round 2 handoff notes
├── Makefile
└── README.md              (this file)
```

---

## Previous Rounds

- W6 SQL Parser — <https://github.com/JYPark-Code/jungle_w6_mini_mysql_sql_parser>
- W7 B+Tree Index DB — <https://github.com/JYPark-Code/jungle_w7_BplusTree_Index_DB>
- W7 final README — [`docs/README_w7.md`](docs/README_w7.md)
