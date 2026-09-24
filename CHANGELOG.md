# Changelog

## 0.1.0 - development

Initial ESDB foundation.

### Runtime

- Added stable opaque-handle C ABI and move-only C++17 facade.
- Pinned SQLite 3.53.4 with hash-verified acquisition.
- Added explicit open flags, busy timeout, journal, synchronous, cache, WAL checkpoint, and foreign-key policy.
- Added transactions, LIFO savepoints, schema-version migrations, integrity checks, online backup, data-version polling, backend capabilities, and health snapshots.
- Added controlled native SQLite handle escape hatch.
- Isolated SQLite vendor diagnostics from ESDB warning settings.

### Store

- Added optional named Store layer with canonical NULL/BOOL/INT32/INT64/DOUBLE/UTF8/BYTES/ARRAY/OBJECT values.
- Added transactional put/get/delete/exists/count.
- Added durable monotonic revisions and synchronous change polling.
- Added pull subscriptions with no asynchronous host callbacks.
- Defined limit == 0 as ESDB_CHANGE_LIMIT_MAX.
- Fixed revision continuity after pruning all change rows and after database reopen.
- Contained C++ change-callback exceptions at the C ABI.

### Adobe integration

- Added ESABI v0.3 ExternalObject adapter.
- Added generation-tagged stale-handle protection.
- Added adapter health/error/data-version/version surfaces without arbitrary SQL.
- Added matching ESFreeMem ownership for returned strings.
- Added minimal ES3-safe extendscript/esdb.jsx facade with guarded unload lifecycle.
- Made staged-path and adapter last-error state thread-local while keeping the generation-tagged handle table synchronized.
- Made JSX adapter loading failure-clean: constructor/ping failures reset facade state and best-effort unload partially loaded ExternalObjects.

### Validation

- Added C and C++ smoke tests.
- Added Runtime/Store hardening tests for transactions, savepoints, migration rollback, canonical values, revision pruning/reopen, signed-64 revision bounds, concurrent Store writes, change-limit semantics, subscriptions, backend capabilities, integrity, and callback containment.
- Removed connection-global last-insert/change-count inference from Store mutation results in favor of statement-local RETURNING semantics.
- Normalized migration callback failures so the returned status and public `esdb_error.status`/phase cannot diverge while preserving callback diagnostics.
- Added ExternalObject adapter smoke tests including stale handles, returned-string release, and concurrent thread-local staging.
- Added CMake presets, installed-package consumer validation, adapter/facade installation, and ZIP packaging.
- Passed ESTC static + live parse and end-to-end facade execution on Illustrator 30.6.0 / ExtendScript 4.5.6.
