# Changelog

## 0.1.0 - 2026-09-24

Initial ESDB foundation.

### Runtime

- Added stable opaque-handle C ABI and move-only C++11+ facade.
- Vendored pinned SQLite 3.53.4 so fresh checkouts build without a system SQLite/network fetch; configure-time SHA-256 verification and a cross-platform SHA3-verified repair tool share one canonical pin.
- Added explicit open flags, busy timeout, journal, synchronous, cache, WAL checkpoint, and foreign-key policy, with strict enum/reserved-field validation and readback verification for explicitly requested SQLite pragmas.
- Added transactions, LIFO savepoints, schema-version migrations, integrity checks, online backup, data-version polling, backend capabilities, and health snapshots.
- Added controlled native SQLite handle escape hatch.
- Isolated SQLite vendor diagnostics from ESDB warning settings.

### Store

- Added optional named Store layer with canonical NULL/BOOL/INT32/INT64/DOUBLE/UTF8/BYTES/ARRAY/OBJECT values.
- Added transactional put/get/delete/exists/count; standalone mutations acquire the SQLite writer slot with `BEGIN IMMEDIATE`, while caller-owned transactions use internal savepoints.
- Added durable monotonic revisions and synchronous change polling.
- Added pull subscriptions with no asynchronous host callbacks; the cursor is atomically published and concurrent/reentrant polls on one subscription fail `ESDB_ERR_BUSY` instead of deadlocking.
- Added read-only Store verification/access without schema-creation side effects; Store mutations fail deterministically on read-only connections.
- Added canonical Store payload and revision-metadata consistency checks so semantic corruption is reported instead of silently coerced.
- Defined limit == 0 as ESDB_CHANGE_LIMIT_MAX.
- Fixed revision continuity after pruning all change rows and after database reopen.
- Contained C++ change-callback exceptions at the C ABI.

### Adobe integration

- Added ESABI 0.3.1 ExternalObject adapter.
- Added generation-tagged stale-handle protection.
- Added adapter health/error/data-version/version surfaces without arbitrary SQL.
- Added matching ESFreeMem ownership for returned strings.
- Added minimal ES3-safe extendscript/esdb.jsx facade with guarded unload lifecycle.
- Made staged-path and adapter last-error state thread-local while keeping the generation-tagged handle table synchronized.
- Made JSX adapter loading failure-clean: constructor/ping failures reset facade state and best-effort unload partially loaded ExternalObjects.

### Validation

- Added C and C++ smoke tests.
- Added Runtime/Store hardening tests for transactions, savepoints, migration rollback, canonical values, revision pruning/reopen, signed-64 revision bounds, concurrent Store writers plus simultaneous same-connection readers, change-limit/snapshot semantics, subscription reentrancy/BUSY behavior, backend capabilities, integrity, and callback containment.
- Added abrupt-process crash/recovery smoke coverage and two-process concurrent WAL writer coverage; the multi-process writer test also passed a 20-repeat stability run after Store writer acquisition was moved to `BEGIN IMMEDIATE`.
- Removed connection-global last-insert/change-count inference from Store mutation results in favor of statement-local RETURNING semantics.
- Serialized invariant-sensitive Store reads against same-connection writers and snapshot change-query rows before invoking callbacks, preventing transient half-applied Store state from escaping under FULLMUTEX concurrency.
- Normalized migration callback failures so the returned status and public `esdb_error.status`/phase cannot diverge while preserving callback diagnostics.
- Added ExternalObject adapter smoke tests including stale handles, returned-string release, and concurrent thread-local staging.
- Added CMake presets, installed-package consumer validation, adapter/facade installation, ZIP packaging, and Windows/Linux/macOS CI definitions.
- Passed ESTC static + live parse and end-to-end facade execution on Illustrator 30.6.0 / ExtendScript 4.5.6.
