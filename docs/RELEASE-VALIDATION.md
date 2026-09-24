# ESDB 0.1.0 validation ledger

This ledger records the evidence used for the current ESDB 0.1.0 release-candidate surface. It separates native tests, packaging checks, parser validation, and live Illustrator execution so one evidence class is not overstated as another.

## Environment

- Windows x64
- Visual Studio 2022 / MSVC 19.44.35228
- SQLite 3.53.4 pinned by `tools/fetch-sqlite.ps1`
- Illustrator 30.6.0
- ExtendScript engine 4.5.6
- ESABI v0.3 integration surface

SQLite's amalgamation acquisition is pinned to the release SHA3-256 recorded in `tools/fetch-sqlite.ps1`. A mismatch is a hard failure.

## Fresh native build

A fresh Visual Studio x64 Release tree was configured with both tests and the ExternalObject adapter enabled.

```text
cmake -S . -B <fresh-build> -G "Visual Studio 17 2022" -A x64
      -DESDB_BUILD_TESTS=ON -DESDB_BUILD_EXTERNALOBJECT=ON
cmake --build <fresh-build> --config Release
ctest --test-dir <fresh-build> -C Release --output-on-failure
```

Result: **5/5 tests passed**.

The suite covers:

- C header/link compatibility;
- C++ move-only facade behavior;
- open/configure/close and health;
- commit, rollback, and LIFO savepoints;
- migration success/failure rollback;
- normalization of migration callback status/error mismatches;
- integrity and online backup;
- canonical Store value round-trips;
- durable Store revision continuity after full change-log pruning and reopen;
- signed-64 Store revision bounds;
- pull change queries and subscriptions;
- C++ callback exception containment at the C ABI;
- Runtime observability of Store failures;
- generation-tagged ExternalObject handle invalidation;
- matching returned-string allocation/free;
- thread-local ExternalObject staging;
- 256 concurrent Store puts from eight threads on one FULLMUTEX connection, with unique revisions and no failed writes.

## Store concurrency mechanism

Store mutations that allocate revisions are serialized by an ESDB Store write mutex. Revision/result discovery does not depend on SQLite connection-global `last_insert_rowid()` or `changes()` state.

Mutation results use statement-local `RETURNING` rows. This is regression-tested under concurrent writers.

## Installed-package consumer

A separate consumer project resolves ESDB exclusively through the installed CMake package:

```text
find_package(esdb 0.1 CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE ESDB::esdb)
```

The consumer successfully:

- includes `<esdb/esdb.h>`;
- includes the installed pinned `<esdb/sqlite3.h>`;
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
SHA256 A819972A99DDE07307837EB93F9F498C7DB44B0E789E84C7162F884EF8FC2401
```

The build-tree and installed copies were byte-identical.

## ExtendScript static and live parse

The shipped `extendscript/esdb.jsx` passes the shared ESTC ES3 static gate.

It also passes ESTC's compile-only live parser gate in the already-running host:

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

The fresh Release DLL and current JSX facade were exercised end-to-end through Illustrator COM without launching or restarting Illustrator.

The live probe successfully performed:

1. absolute-path ExternalObject DLL load;
2. ESDB version query -> `0.1.0`;
3. SQLite version query -> `3.53.4`;
4. ABI version query -> `1`;
5. database open;
6. health snapshot;
7. `PRAGMA data_version` query;
8. handle-count verification while open -> `1`;
9. database close;
10. handle-count verification after close -> `0`;
11. guarded facade unload;
12. `loadedSpec` reset to null.

The probe result was `ok: true`.

A separate recovery probe deliberately attempted to load a missing DLL. It verified that the failure throws, leaves no published bridge/spec state, and the same facade can immediately recover by loading the valid adapter and unloading successfully.

Adobe can keep a loaded DLL image mapped until Illustrator exits even after `ExternalObject.unload()`; this is a host behavior and is why development rebuilds should use a fresh DLL path or filename while Illustrator remains open.

## Packaging

CPack ZIP packaging includes:

- native static library;
- ESDB ExternalObject DLL/import library on Windows;
- public C/C++ headers;
- pinned SQLite declaration header;
- CMake package config/targets;
- ES3 JSX facade;
- README, changelog, architecture/ABI/Store/compression docs;
- license.

Generated build/install/CPack staging state and release ZIPs are excluded from Git.

## Remaining qualification boundaries

The current evidence does **not** claim:

- transparent compression support;
- a full Store API exposed through ExternalObject/JSX;
- live compatibility on Illustrator versions other than 30.6.0 / ExtendScript 4.5.6;
- exhaustive power-loss/crash-injection qualification;
- exhaustive multi-process stress qualification for future non-stock storage backends.

Plain SQLite remains the semantic/reference backend until any compressed backend independently passes the documented correctness and compatibility gates.
