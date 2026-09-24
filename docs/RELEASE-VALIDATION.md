# ESDB 0.1.0 validation ledger

This ledger records the evidence used for the ESDB 0.1.0 release surface. It separates native tests, packaging checks, parser validation, and live Illustrator execution so one evidence class is not overstated as another.

## Environment

- Windows x64
- Visual Studio 2022 / MSVC 19.44.35228
- SQLite 3.53.4 vendored and pinned by `cmake/sqlite-pin.json`
- Illustrator 30.6.0
- ExtendScript engine 4.5.6
- ESABI 0.3.1 at release commit `65c9c3ce627a26a89d6bf90547678841df0cf981`

`cmake/sqlite-pin.json` is the single machine-readable SQLite pin: upstream URL + release SHA3-256 + per-file SHA-256 digests. CMake verifies the vendored amalgamation on every configure; `tools/fetch-sqlite.cmake` and the PowerShell wrapper consume that same pin. A mismatch is a hard failure.

## Fresh native build

A fresh Visual Studio x64 Release tree was configured with both tests and the ExternalObject adapter enabled.

```text
cmake -S . -B <fresh-build> -G "Visual Studio 17 2022" -A x64
      -DESDB_BUILD_TESTS=ON -DESDB_BUILD_EXTERNALOBJECT=ON
cmake --build <fresh-build> --config Release
ctest --test-dir <fresh-build> -C Release --output-on-failure
```

Result: **7/7 tests passed**.

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

## Store concurrency mechanism

Store mutations that allocate revisions are serialized by an ESDB Store connection mutex. Revision/result discovery does not depend on SQLite connection-global `last_insert_rowid()` or `changes()` state.

Mutation results use statement-local `RETURNING` rows. Standalone Store mutations acquire the SQLite writer slot with `BEGIN IMMEDIATE`; inside a caller-owned transaction they use an internal savepoint. Store reads that depend on the multi-statement record/change/revision invariant serialize against writers on the same connection. Change polling copies a coherent bounded snapshot while serialized, releases the Store mutex, and only then invokes user callbacks, so callback execution never holds the Store mutation lock. This is regression-tested under concurrent writers and simultaneous Store readers.

The two-process WAL writer test initially exposed deterministic `SQLITE_BUSY` on a deferred writer upgrade. Moving standalone Store mutation acquisition to `BEGIN IMMEDIATE` routed contention through SQLite's configured busy handler before mutation work starts. Against the final Release build, both the corrected two-process writer test and abrupt-process crash/recovery test passed **30 consecutive repetitions** in addition to the full suite.

## Installed-package consumer

A separate consumer project resolves ESDB exclusively through the installed CMake package in both C++ and pure C configurations:

```text
find_package(esdb 0.1 CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE ESDB::esdb)
```

The consumer successfully:

- builds and runs a C++ consumer linked only through `ESDB::esdb`;
- builds and runs a pure C consumer linked through the same exported target;
- includes `<esdb/esdb.h>` and the installed pinned `<esdb/sqlite3.h>`;
- opens an in-memory ESDB database;
- obtains the controlled native `sqlite3*`;
- verifies the native SQLite version matches `esdb_sqlite_version()`.

The exported package resolves its public Threads dependency through `find_dependency(Threads)`.

## ExternalObject binary

The Release `ESDB.dll` is PE x64 and exports exactly the expected 15 named entry points:

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

The validated Release DLL hash is:

```text
SHA256 662AFDEBAF8BAF561F6A5317F92BA219AB9F5E34F2E0EF2A270519FFEF341FE1
```

The build-tree and installed copies were byte-identical.

The PE DLL is built with MSVC reproducibility flags. Two independent clean build directories produced the same `ESDB.dll` SHA-256 shown above. The static archive is not used as a cross-build reproducibility gate: MSVC object metadata still differs across distinct build directories even when librarian timestamps/member paths are normalized.

## ExtendScript static and live parse

The shipped `extendscript/esdb.jsx` is an include-style bundle (no `#target` line), so the shared ESTC ES3 static gate is run in embedded-bundle mode:

```text
estc check extendscript/esdb.jsx --no-target
PASS: extendscript/esdb.jsx [acorn-ecma3]
```

It also passes ESTC's compile-only live parser gate against the available Illustrator COM host:

```text
estc check extendscript/esdb.jsx --no-target --live
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

The fresh Release DLL and current JSX facade were exercised end-to-end through the available Illustrator COM host using `tools/live-externalobject-probe.ps1` under Windows PowerShell (the probe uses the .NET Framework `Marshal.GetActiveObject` API, which is unavailable in PowerShell 7). It was run without `-Launch`, attaching to the running Illustrator instance; the probe does not terminate the host, and it uses a uniquely named temporary DLL copy so Adobe's per-filename native-module cache cannot turn a live test into stale-binary evidence.

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

The ZIP generator is given a release-specific `SOURCE_DATE_EPOCH` through `cmake/CPackProjectConfig.cmake`, so archive entry timestamps are stable. Two package generations separated in wall-clock time produced byte-identical ZIPs. CI repeats packaging and fails if the SHA-256 changes. Generated build/install/CPack staging state, release ZIPs, and their checksum sidecars are excluded from Git. The final ZIP digest is reported alongside the artifact rather than embedded here, because this ledger itself is packaged and embedding the package digest would make the archive hash self-referential.

## Remaining qualification boundaries

The current evidence does **not** claim:

- transparent compression support;
- a full Store API exposed through ExternalObject/JSX;
- live compatibility on Illustrator versions other than 30.6.0 / ExtendScript 4.5.6;
- exhaustive power-loss/crash-injection qualification;
- exhaustive long-duration/many-process stress qualification for either the stock backend or future non-stock storage backends.

Plain SQLite remains the semantic/reference backend until any compressed backend independently passes the documented correctness and compatibility gates.
