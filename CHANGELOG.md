# Changelog

## 0.2.0 - 2026-09-25

Initial ESDB foundation.

### Runtime

- Added stable opaque-handle C ABI and move-only C++11+ facade.
- Vendored pinned SQLite 3.53.4 so fresh checkouts build without a system SQLite/network fetch; configure-time SHA-256 verification and a cross-platform SHA3-verified repair tool share one canonical pin.
- Added explicit open flags, busy timeout, journal, synchronous, cache, WAL checkpoint, and foreign-key policy, with strict enum/reserved-field validation and readback verification for explicitly requested SQLite pragmas.
- Added transactions, LIFO savepoints, schema-version migrations, integrity checks, online backup, data-version polling, backend capabilities, and health snapshots.
- Added controlled native SQLite handle escape hatch.
- On Windows, `ESDBCore.dll` now exports the pinned SQLite public API so native-handle consumers link against the same SQLite image that owns the handle rather than introducing a second SQLite build.
- Isolated SQLite vendor diagnostics from ESDB warning settings.

### Store

- Added optional named Store layer with canonical NULL/BOOL/INT32/INT64/DOUBLE/UTF8/BYTES/ARRAY/OBJECT values.
- Added transactional put/get/delete/exists/count; standalone mutations acquire the SQLite writer slot with `BEGIN IMMEDIATE`, while caller-owned transactions use internal savepoints.
- Added durable monotonic revisions and synchronous change polling.
- Added pull subscriptions with no asynchronous host callbacks; the cursor is atomically published and concurrent/reentrant polls on one subscription fail `ESDB_ERR_BUSY` instead of deadlocking.
- Added read-only Store verification/access without schema-creation side effects; Store mutations fail deterministically on read-only connections.
- Added canonical Store payload and revision-metadata consistency checks so semantic corruption is reported instead of silently coerced.
- Treat Store key misses as normal `ESDB_ERR_NOT_FOUND` control flow without incrementing database health error counters or replacing the connection's last-error snapshot.
- Defined limit == 0 as ESDB_OBJECT_CHANGE_LIMIT_MAX.
- Fixed revision continuity after pruning all change rows and after database reopen.
- Contained C++ change-callback exceptions at the C ABI.

### ORM compiler

- Added the optional compiler-oriented ORM lane: pinned `drizzle-orm@0.45.3` schema extraction into sealed `esdb.ir/v1`, deterministic C++11/TypeScript/ES3/ESABI generation, and named prepared-statement repositories whose generated path does not transport SQL text.
- Added pinned `drizzle-kit@0.31.11` production migration packaging with deterministic LF normalization, SHA-256 provenance, migration SQL policy linting, explicit destructive-change acknowledgements, generated C++11 `esdb_migration[]` wrappers, and `PRAGMA user_version` as the runtime ledger.
- Added logical-vs-physical identifier preservation (for example `createdAt -> created_at`) and independent public API vs SQLite bind ordering so ergonomic UPDATE signatures cannot corrupt placeholder order.
- Added strict IR hardening for generated-name collisions/reserved prefixes, metadata injection, canonical int64/default values, NUL text defaults, keyed mutation requirements, and deterministic sort order.
- Added multi-table schema-wide ES3/bridge operation names qualified by table, while preserving repository-local C++ methods and existing single-table User names; exact nullable INTEGER TypeScript rows retain `| null`.
- Added a Workmark-derived relational compiler fixture covering multi-table extraction, composite foreign keys, indexes, CHECKs, joins, upserts, exact int64, nullable columns, and deterministic cross-language code generation.
- Added strict U1 ES3 wire handling with exact int64 decimal lanes, byte-exact text/blob transport, generated named-operation bridge contracts, and a concrete generation-tagged ESABI User bridge that uses only named generated operations.
- Added executable User reference tooling with deterministic regeneration/drift checks, native migration/CRUD smoke, concrete ESABI bridge smoke, pinned ESTC static validation, compile-only live parsing, and end-to-end Illustrator 30.6.0 / ExtendScript 4.5.6 ExternalObject execution.
- Fixed empty non-null BLOB binding so generated C++ uses `sqlite3_bind_zeroblob(..., 0)` instead of accidentally binding SQL NULL.
- Hardened migration input handling against invalid UTF-8, BOMs, U+0000, unterminated SQL strings/comments/quoted identifiers, and malformed destructive-acknowledgement source paths.

### Adobe integration

- Added ESABI 0.3.1 ExternalObject adapter.
- Added generation-tagged stale-handle protection.
- Added adapter health/error/data-version/version surfaces and the bounded typed single-statement `querySql` escape hatch.
- Added canonical transaction and typed Store methods to the ESABI adapter; adapter slots own at most one native transaction and roll it back on close/termination when necessary.
- Added `esdb_query()` / JSX `Database.query()` and `run()`: SQL text and typed parameters cross separately, exact INT64/BYTES survive the Adobe wire, caller values are bound natively, results are bounded typed Q1 packets, and production DDL authority remains migration-owned.
- Kept raw-query result metadata deterministic for zero-row result sets and made decoded row values exception-safe when a query callback throws.
- Added byte-exact ASCII-hex transport for database paths, Store names/keys, UTF-8/structured values, and BYTES, plus canonical decimal strings for INT64/revisions/counts so ES3 never rounds 64-bit state.
- Added matching ESFreeMem ownership for returned strings.
- Added ES3-safe Store facade with get/set/patch, transactions, exact INT64/BYTES wrappers, change polling, key-filtered pull subscriptions, pruning, and guarded unload lifecycle.
- Added optional structured-codec integration: ESON is auto-detected when present but remains an unbundled peer, preserving ESDB's MIT license boundary.
- Made staged-path and adapter last-error state thread-local while keeping the generation-tagged handle table synchronized.
- Made JSX adapter loading failure-clean: constructor/ping failures reset facade state and best-effort unload partially loaded ExternalObjects.

### Validation

- Added C and C++ smoke tests.
- Added Runtime/Store hardening tests for transactions, savepoints, migration rollback, canonical values, revision pruning/reopen, signed-64 revision bounds, concurrent Store writers plus simultaneous same-connection readers, change-limit/snapshot semantics, subscription reentrancy/BUSY behavior, backend capabilities, integrity, and callback containment.
- Added abrupt-process crash/recovery smoke coverage and two-process concurrent WAL writer coverage; both tests passed 30 consecutive repetitions against the final Release build after Store writer acquisition was moved to `BEGIN IMMEDIATE`.
- Removed connection-global last-insert/change-count inference from Store mutation results in favor of statement-local RETURNING semantics.
- Serialized invariant-sensitive Store reads against same-connection writers and snapshot change-query rows before invoking callbacks, preventing transient half-applied Store state from escaping under FULLMUTEX concurrency.
- Normalized migration callback failures so the returned status and public `esdb_error.status`/phase cannot diverge while preserving callback diagnostics.
- Added ExternalObject adapter smoke tests including stale handles, returned-string release, concurrent thread-local staging, typed Store round-trips, exact >2^53 INT64 transport, transaction commit/rollback, change windows, and pruning.
- Added CMake presets, installed-package C++/C/generated-ORM consumer validation, embedded Runtime consumer validation, adapter/facade installation, and Windows/Linux/macOS CI definitions.
- Added reproducible Release ZIP packaging: fixed release epoch plus post-build ZIP timestamp normalization without recompressing payloads; Python 3 is a packaging-only dependency and CI compares two independently generated archive hashes.
- Passed ESTC static + live parse and end-to-end facade execution on Illustrator 30.6.0 / ExtendScript 4.5.6, including an astral-Unicode database path/store/key, embedded U+0000 UTF-8 value, exact INT64, BYTES, ESON structured objects, patch/transaction rollback+commit, pull subscriptions, pruning, stale-handle rejection, and guarded unload.
