# ESDB 0.1.0 validation ledger

This ledger records evidence for the ESDB 0.1.0 release surface across successive validation cuts. It separates native tests, packaging checks, parser validation, and live Illustrator execution so one evidence class is not overstated as another. The original binary fingerprint and its export list below are historical; the current working tree has since added typed raw SQL and additional adapter methods.

## Environment

- Windows x64
- Visual Studio 2022 / MSVC 19.44.35228
- SQLite 3.53.4 vendored and pinned by `cmake/sqlite-pin.json`
- Illustrator 30.6.0
- ExtendScript engine 4.5.6
- ESABI 0.3.1 at source commit `400fefa14c1c09c0e51555f8e834975bdddbdb1d`
- Node.js 22.23.2 / npm 10.9.8 for the local ORM toolchain validation; CI pins Node.js 22.23.3
- `drizzle-orm@0.45.3`, `drizzle-kit@0.31.11`, TypeScript 5.8.3

## Current live-tree integration status (2026-09-25)

The current dirty source tree has been revalidated on Windows/MSVC after the typed raw-SQL and Workmark
relational work:

- ExternalObject-enabled native build and CTest: **8/8 passed**.
- ORM compiler suite: **13 passed / 0 failed / 0 skipped**; `orm/drizzle` and the User example type/drift
  checks pass.
- Sealed User IR, generated artifacts, and migration package checks pass; the Workmark-derived relational
  test exercises public Drizzle extraction, generated C++/TS/ES3 contracts, and deterministic generation.
- ESTC conservative static checks pass for the shipped ESDB facade and generated User ES3 repository.
- A fresh install, all three package consumers, and the embedded consumer pass with their installed/build
  `ESDBCore.dll` directories on PATH; CI now adds the matching directories before execution.
- The current adapter build exports **45** exact named methods. This local build used the available sibling
  ESABI checkout; CI independently checks out its immutable ESABI pin.
- Two CPack generations of the current build are byte-identical; the package digest remains attached to
  the generated artifact rather than embedded in this ledger.
- MSVC `/W4 /WX /std:c++14` syntax checks pass for the generated User repository/migrations/bridge and the
  Workmark-derived repository. The exact C++11 Clang gate is present in Ubuntu CI; local Clang selected
  MSVC standard-library headers that require C++14, so that exact compiler mode was not independently
  reproduced on Windows.
- With explicit launch approval, `tools/live-externalobject-probe.ps1` launched Illustrator, passed, and
  closed the launched instance without touching a document:
  `OK|30.6.0|4.5.6|esdb=0.1.0|sqlite=3.53.4|dataVersion=1|memoryStore=1|objectStore=1|persistence=1|unicode=1|int64=1|bytes=1|rawSql=1|rawBind=1|rawExactInt64=1|rawBool=1|rawInt32=1|rawEmptyMetadata=1|rawTruncate=1|rawInjectionSafe=1|rawRollback=1|tx=1|subscription=1|prune=1|recovery=1|unload=1|stale=1`.

`cmake/sqlite-pin.json` is the single machine-readable SQLite pin: upstream URL + release SHA3-256 + per-file SHA-256 digests. CMake verifies the vendored amalgamation on every configure; `tools/fetch-sqlite.cmake` and the PowerShell wrapper consume that same pin. A mismatch is a hard failure.

## Fresh native build

A fresh Visual Studio x64 Release tree was configured with both tests and the ExternalObject adapter enabled.

```text
cmake -S . -B <fresh-build> -G "Visual Studio 17 2022" -A x64
      -DESDB_BUILD_TESTS=ON -DESDB_BUILD_EXTERNALOBJECT=ON
cmake --build <fresh-build> --config Release
ctest --test-dir <fresh-build> -C Release --output-on-failure
```

The original Runtime-only suite passed **7/7 tests**. The current ExternalObject-enabled integration suite
passes **8/8 tests** (including `esdb_externalobject_smoke`).

The suite covers:

- C header/link compatibility;
- C++ move-only facade behavior;
- open/configure/close and health;
- commit, rollback, and LIFO savepoints;
- migration success/failure rollback;
- normalization of migration callback status/error mismatches;
- integrity and online backup;
- abrupt-process WAL recovery for both an uncommitted mutation and an immediately committed mutation;
- two-process concurrent WAL writers with revision/count/change-journal verification;
- canonical Store value round-trips;
- durable Store revision continuity after full change-log pruning and reopen;
- signed-64 Store revision bounds;
- pull change queries and subscriptions;
- C++ callback exception containment at the C ABI;
- Runtime observability of Store failures;
- generation-tagged ExternalObject handle invalidation;
- matching returned-string allocation/free;
- thread-local ExternalObject staging;
- 256 concurrent Store puts from eight threads on one FULLMUTEX connection, with unique revisions and no failed writes;
- concurrent Store revision/count readers running while another thread performs 160 writes, with no transient metadata inconsistency or read failure;
- subscription callback reentrancy returning `ESDB_ERR_BUSY` rather than deadlocking.

## Durable ObjectStore concurrency mechanism

ObjectStore mutations that allocate revisions are serialized by an ESDB connection mutex. Revision/result discovery does not depend on SQLite connection-global `last_insert_rowid()` or `changes()` state.

Mutation results use statement-local `RETURNING` rows. Standalone ObjectStore mutations acquire the SQLite writer slot with `BEGIN IMMEDIATE`; inside a caller-owned transaction they use an internal savepoint. ObjectStore reads that depend on the multi-statement record/change/revision invariant serialize against writers on the same connection. Change polling copies a coherent bounded snapshot while serialized, releases the Store mutex, and only then invokes user callbacks, so callback execution never holds the ObjectStore mutation lock. This is regression-tested under concurrent writers and simultaneous Store readers.

The two-process WAL writer test initially exposed deterministic `SQLITE_BUSY` on a deferred writer upgrade. Moving standalone ObjectStore mutation acquisition to `BEGIN IMMEDIATE` routed contention through SQLite's configured busy handler before mutation work starts. Against the final Release build, both the corrected two-process writer test and abrupt-process crash/recovery test passed **30 consecutive repetitions** in addition to the full suite.

## Installed-package consumer

A separate consumer project resolves ESDB exclusively through the installed CMake package in both C++ and pure C configurations:

```text
find_package(esdb 0.1 CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE ESDB::esdb)
```

The consumer successfully, with the installed `bin` directory on Windows' DLL search path:

- builds and runs a C++ consumer linked only through `ESDB::esdb`;
- builds and runs a pure C consumer linked through the same exported target;
- builds and runs the generated User ORM repository/migration wrapper against the installed package, proving generated code consumes only the installed ESDB Runtime + pinned SQLite declaration/API;
- includes `<esdb/esdb.h>` and the installed pinned `<esdb/sqlite3.h>`;
- opens an in-memory ESDB database;
- obtains the controlled native `sqlite3*`;
- calls the pinned SQLite API while linking only `ESDB::esdb` (including through
  `ESDBCore.dll`'s SQLite exports on Windows, never a second SQLite image);
- verifies the native SQLite version matches `esdb_sqlite_version()`.

The exported package resolves its public Threads dependency through `find_dependency(Threads)`.

## ORM compiler and generated native bridge

The optional ORM lane was validated independently from the base Runtime/Store
suite so generated-code evidence is not conflated with handwritten Runtime
tests.

The deterministic compiler/wire suite was run from the repository root:

```text
npm run check --prefix orm/drizzle
npm run check --prefix examples/orm/user
node tests/orm/run.mjs
```

Result: **13/13 ORM tests passed**. The suite covers canonical IR hashing,
deterministic model generation/drift detection, C++11 syntax, Drizzle public
metadata extraction, strict ES3 U1 wire parsing, IR hardening, pinned Drizzle
Kit migration packaging/linting/acknowledgement, SQLite-semantic physical-schema
hashing/differential, explicit adoption baseline/report behavior, multi-table
FK/CHECK/index semantics, compile-time joins, deterministic upsert, canonical
SQL generation, and cross-language type mapping. Migration hardening includes
invalid UTF-8, BOM, U+0000, unterminated SQL strings/comments/quoted identifiers,
transaction/Runtime-owned SQL rejection, canonical destructive-acknowledgement
paths, and physical-schema provenance for baseline-zero packages.

The checked-in User slice also passed its independent drift gate:

```text
drizzle schema -> re-extracted IR -> seal -> byte comparison
generated bindings --check
packaged Drizzle Kit migrations --check
```

The sealed User IR hash was:

```text
f619d9734d3872bd3b788d50799a02a82ac83d026cb824ae408ba3588c86c5ee
```

The pinned baseline-zero Kit chain also realizes to the deterministic physical
schema identity:

```text
normalizer: sqlite-introspection-v1
sha256:93f3dd93905038e0e5b7d487667a1516dae717a0a767f37ee8feb77de963ca81
```

That identity is carried in migration manifest v2 and the generated C++
migration wrapper. Nonzero/adopted baselines intentionally record the physical
hash as unresolved until the external baseline schema is supplied, rather than
hashing only the tail migrations and pretending it describes the whole DB.

A Visual Studio 2022 x64 Release build of the standalone User example then ran
the real generated repository against ESDB Runtime:

```text
ESDB ORM user smoke PASS
```

That smoke applies the generated Drizzle Kit migration wrapper twice, verifies
the target `PRAGMA user_version`, checks repository/migration IR-hash
coherence, executes insert/find/update/list/delete through generated prepared
statements, and round-trips `INT64_MAX`.

The concrete User ESABI bridge was also compiled, linked, and executed:

```text
ESDB ORM concrete bridge smoke PASS
```

That host-independent native smoke calls the bridge through real ESABI values
and validates exported-signature discovery, ping/version handshake,
migration-on-open, strict U1 parameter parsing, CRUD, ergonomic UPDATE argument
mapping vs SQLite bind order, generation-tagged close/stale-handle rejection,
and reopen/migration idempotence.

The generated User ES3 facade then passed both ESTC's conservative ES3 static
gate and its compile-only live parser against Illustrator 30.6.0 /
ExtendScript 4.5.6. Finally, `tools/live-orm-user-probe.ps1` loaded a uniquely
named copy of the concrete `UserOrmBridge.dll` plus side-by-side
`ESDBCore.dll` through Illustrator's real `ExternalObject` loader and
executed the generated facade end to end. The live result was:

```text
OK|30.6.0|4.5.6|orm=1|crud=1|update=1|int64=1|stale=1|unload=1
```

The live probe covers migration-on-open, safe-integer CRUD, UPDATE ordering,
exact >2^53 / INT64_MAX-value transport in exact mode, list decoding,
unload-while-open rejection, generation-tagged stale-handle rejection, close,
and unload. The bridge uses ESDB's reusable Windows delay-load hook so
`ESDBCore.dll` is resolved relative to the product bridge DLL rather than
Illustrator's process-wide DLL search path.

The generated repository borrows `sqlite3*` through
`esdb_native_handle()`. On Windows, `ESDBCore.dll` now exports the pinned
SQLite public API. A clean install plus separate C and C++ package-consumer
build/run passed while linking only `ESDB::esdb`, demonstrating that
native-handle callers resolve SQLite against the same ESDB-owned SQLite image
rather than a second SQLite build.

The ORM CI lanes pin Node.js 22.23.3, install both lockfile-defined
dependency sets with `npm ci`, check out the exact ESABI source commit, and run
the compiler/drift suites on both Ubuntu and Windows. Ubuntu additionally
compiles generated C++/ESABI sources with C++11 warnings as errors; both Ubuntu
and Visual Studio x64 Release build and run the native repository and concrete
bridge smokes. Ubuntu also builds and executes the generated-vs-handwritten ORM
benchmark harness as a non-timing-gating health check so benchmark code cannot
silently rot on shared runners.

### ORM performance evidence

The original Release run of the harness in `bench/` compared the generated User
repository with a separately linked handwritten SQLite prepared-statement
baseline using the same schema, SQL semantics, transaction modes, warmup, and
seven internal timing samples. Five independent process repetitions produced
these median generated/handwritten results on the validation machine:

| Metric | Generated | Handwritten | Ratio |
|---|---:|---:|---:|
| point read | 2.082 us/op | 2.109 us/op | 0.988x |
| point write | 0.407 us/op | 0.391 us/op | 1.041x |
| 10k-row scan | 2.042 ms | 2.096 ms | 0.974x |
| bulk insert | 952,236 rows/s | 959,168 rows/s | 0.993x |
| cold prepare / statement | 9.53 us | 8.70 us | 1.096x |
| statement memory | 11,800 B | 11,792 B | ~1.001x |
| executable size | 45,568 B | 43,008 B | 1.060x |
| object size | 299,966 B | 269,063 B | 1.115x |

The identical prepare-each point-read control measured about 2.174 us/op. The
generated persistent path measured 2.082 us/op, a ~1.044x reuse speedup in this
hot-cache workload. Those initial samples met the provisional latency budgets. Later expanded interleaved runs exposed severe
host-scheduling noise in scan timings; the current harness reports scan stability and does not treat an
unstable scan ratio as a pass.

The current interleaved harness was rebuilt and run with both `--repeats 5` and
`--repeats 9` on Windows/MSVC 19.44 / SQLite 3.53.4. The prepare-each control
now binds the same ID and fully decodes the same four-column row as the
persistent paths. Point reads and scans consume every decoded field through a
checksum; generated and handwritten checksums matched exactly
(`3188731244`). The 9-process run uses 15 internal scan samples per
implementation and balanced generated/handwritten execution order. Values below
are the 9-run process medians; spread is the process-median relative range.

| Metric | Generated | Handwritten | Ratio | Spread G/H |
|---|---:|---:|---:|---:|
| Point read | 2.171 us/op | 2.188 us/op | 0.992x | 15.3% / 7.1% |
| Point write | 0.395 us/op | 0.395 us/op | 0.998x | 9.1% / 7.2% |
| 10k-row scan | 2.384 ms | 2.450 ms | 0.973x | 11.7% / 11.1% |
| Bulk insert | 890,028 rows/s | 912,392 rows/s | 0.975x | 8.7% / 18.3% |
| Prepare total | 65.3 us | 60.8 us | 1.074x | 108.9% / 86.7% |
| Prepare per statement | 10.883 us | 10.133 us | 1.074x | 108.9% / 86.7% |
| SQLite statement memory | 11,800 B | 11,800 B | 1.000x | 0% / 0% |
| Executable | 47,616 B | 45,568 B | 1.045x | - |
| Object file | 319,078 B | 298,036 B | 1.071x | - |

Statement count and reuse were identical (6 statements; reuse true). The scan
stability check is **STABLE** on this 9-process run: the maximum process-median
spread is 11.7%, below the configured 20% ceiling, and the 0.973x ratio passes
the provisional 1.10x scan budget. Pooled internal scan samples still contain
scheduler outliers (77.5% generated / 78.9% handwritten range), which is why
the gate uses the distribution of process medians and keeps reporting the raw
spread. The 5-run check remains unstable by policy because it is below the
nine-process minimum. `llvm-size` measured executable `.text` at 31,506 / 30,162
B and object `.text` at 44,636 / 42,031 B (generated / handwritten); the
produced executable file-size ratio is 1.045x.

The same fair-decoding harness also passed on hosted Ubuntu CI
(run 36200107422, 9 processes, balanced order, checksums matched exactly at
`3188731244`): point read 1.802 / 1.783 us (1.011x), point write
0.234 / 0.232 us (1.010x), 10k-row scan 1.413 / 1.416 ms (0.998x, process
spread 2.9% / 4.5%, stable), bulk insert 1,675,505 / 1,712,336 rows/s
(0.978x), prepare per statement 7.146 / 6.872 us (1.040x), prepare-each
1.481 / 1.480 us (1.001x), statement memory 11,800 / 11,800 B, executable
63,856 / 58,456 B (1.092x), object 80,864 / 62,152 B (1.301x). All
provisional budgets passed there as well. The Linux object-file ratio is
larger than the Windows one because the two platforms count different
object/metadata content; the executed `.text` comparison above is the
nearer code-size signal, and neither platform showed a latency overhead
requiring generator changes.

## Typed raw-SQL application-query surface

`esdb_query()` and ExternalObject `querySql()` are implemented as an explicit
application-query escape hatch outside generated ORM v1. ExtendScript passes SQL
text as UTF-8 encoded ASCII hex and parameters in a separate typed `P1` packet;
the Runtime binds caller values and never interpolates them. Replies use the
bounded `Q1` typed result packet, including column metadata when a query returns
zero rows. This surface is not migration authority: production DDL/diff remains
owned by the application's migration system (Drizzle Kit for the ESDB ORM).

Current Runtime and ExternalObject smoke coverage passes for NULL, BOOL, INT32,
exact INT64 (including values outside ±2^53 and signed bounds), REAL, UTF-8 TEXT,
and byte-exact BLOB; malformed SQL/packets, invalid UTF-8, placeholder-count
mismatch, multiple statements, row/byte caps, truncation, and callback failures
are also exercised. The corresponding ES3 source passes the conservative ESTC
static gate. The current live Illustrator probe additionally passed typed
`Database.query()` / `run()` operations, exact int64, injection resistance,
transactions, truncation, and zero-row column metadata on Illustrator 30.6.0 /
ExtendScript 4.5.6.

The concrete native ESABI bridge was separately measured twice in Release at
approximately 1.92 us native `findById` versus 2.86 us through the bridge,
about **0.93-0.95 us absolute bridge overhead** and **1.48-1.50x total
latency** for the tiny User row.

The real Illustrator/ExtendScript benchmark also completed twice with zero
rejected samples. Corrected end-to-end medians were **253.631 us/op** and
**382.124 us/op** for 1,000 finds. The second run had substantially higher host
spread, so Illustrator timing is retained as host-level execution evidence,
not promoted to a hard release performance budget.

## ExternalObject binary

The original 0.1.0 Release `ESDB.dll` was PE x64 and exported the following 15 named entry points:

```text
ESFreeMem
ESGetVersion
ESInitialize
ESTerminate
abiVersion
close
dataVersion
handleCount
health
lastError
openStaged
ping
sqliteVersion
stage
version
```

The original Release DLL hash was:

```text
SHA256 662AFDEBAF8BAF561F6A5317F92BA219AB9F5E34F2E0EF2A270519FFEF341FE1
```

The build-tree and installed copies for that historical cut were byte-identical. The current source has since expanded the adapter contract; its current export verifier checks 45 exact names, including
`querySql`, transaction, durable ObjectStore, and process-memory Store methods. The historical hash below
does not fingerprint the current dirty source tree.

The PE DLL is built with MSVC reproducibility flags. Two independent clean build directories produced the same `ESDB.dll` SHA-256 shown above. The static archive is not used as a cross-build reproducibility gate: MSVC object metadata still differs across distinct build directories even when librarian timestamps/member paths are normalized.

## ExtendScript static and live parse

The shipped `extendscript/esdb.jsx` passes the shared ESTC ES3 static gate.

It also passes ESTC's compile-only live parser gate against the available Illustrator COM host:

```text
Illustrator 30.6.0
ExtendScript 4.5.6
result: OK
```

The validated facade SHA-256 is:

```text
6423B206AFCA138983AFC886778FEC7392090217244B8272B9F3DBF1098828C8
```

The source and installed facade copies were byte-identical.

## Live Illustrator runtime

The fresh Release DLL and current JSX facade were exercised end-to-end through the available Illustrator COM host using `tools/live-externalobject-probe.ps1`. The probe does not restart or terminate Illustrator; it uses a uniquely named temporary DLL copy so Adobe's per-filename native-module cache cannot turn a live test into stale-binary evidence.

The live probe successfully performed:

1. fresh uniquely named DLL copy loaded through a temporary ExternalObject search folder, avoiding Illustrator's per-filename development cache;
2. deliberate missing-library load failure with no leaked facade bridge/spec state;
3. immediate recovery by loading the valid adapter;
4. ESDB version query -> `0.1.0`;
5. SQLite version query -> `3.53.4`;
6. ABI version query -> `1`;
7. database open;
8. health snapshot;
9. `PRAGMA data_version` query;
10. handle-count verification while open -> `1`;
11. facade unload rejection while a database handle is still open, without disturbing that handle;
12. database close and handle-count return to `0`;
13. stale generation-tagged handle rejection with adapter error code `5`;
14. guarded facade unload;
15. `loadedSpec` reset to null.

The live result was:

```text
OK|30.6.0|4.5.6|esdb=0.1.0|sqlite=3.53.4|dataVersion=1|recovery=1|unload=1|stale=1
```

Adobe can keep a loaded DLL image mapped until Illustrator exits even after `ExternalObject.unload()`; this is a host behavior and is why development rebuilds should use a fresh DLL path or filename while Illustrator remains open.

## Packaging

CPack ZIP packaging includes:

- native static library;
- ESDB ExternalObject DLL/import library on Windows;
- public C/C++ headers;
- pinned SQLite declaration header;
- CMake package config/targets;
- ES3 JSX facade;
- README, changelog, architecture/ABI/Store/compression/release-validation docs;
- license and third-party notices.

Release packaging uses a fixed `SOURCE_DATE_EPOCH` from `cmake/CPackProjectConfig.cmake` and a CPack post-build normalizer (`cmake/CPackNormalize.cmake` + `tools/normalize-zip.py`). The normalizer patches DOS timestamps in both local and central ZIP headers without recompressing payloads; this also removes CPack's wall-clock directory-entry timestamps. Two package generations separated in wall-clock time produced byte-identical ZIPs. CI repeats packaging and fails if the SHA-256 changes. Python 3 is therefore a release-packaging dependency, not a core build/runtime dependency. Generated build/install/CPack staging state, release ZIPs, and their checksum sidecars are excluded from Git. The final ZIP digest is reported alongside the artifact rather than embedded here, because this ledger itself is packaged and embedding the package digest would make the archive hash self-referential.

## Remaining qualification boundaries

The current evidence does **not** claim:

- transparent compression support;

- live compatibility on Illustrator versions other than 30.6.0 / ExtendScript 4.5.6;
- exhaustive power-loss/crash-injection qualification;
- exhaustive long-duration/many-process stress qualification for either the stock backend or future non-stock storage backends;
- live ORM bridge compatibility on Illustrator/ExtendScript versions other than the validated Illustrator 30.6.0 / ExtendScript 4.5.6 pair;
- a convenience C++ `esdb_orm_verify()` helper (read-only CLI physical hash/diff/adoption verification is implemented);
- final cross-machine benchmark thresholds or a hard live-Illustrator latency budget.

Plain SQLite remains the semantic/reference backend until any compressed backend independently passes the documented correctness and compatibility gates.
