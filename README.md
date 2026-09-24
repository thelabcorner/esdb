<div align="center">

# ESDB: Native application state and durable storage for Adobe tooling

### SQLite-backed Runtime + optional Store API + ESABI ExternalObject adapter

[![Version](https://img.shields.io/badge/version-0.1.0--dev-orange)](#status)
[![SQLite](https://img.shields.io/badge/SQLite-3.53.4-blue)](https://www.sqlite.org/)
[![C ABI](https://img.shields.io/badge/API-C%20ABI%20%2B%20C%2B%2B17-success)](#native-api)
[![ExtendScript](https://img.shields.io/badge/Illustrator%2030.6-live%20validated-success)](#extendscript)
[![Compression](https://img.shields.io/badge/compression-qualification%20pending-lightgrey)](docs/COMPRESSION.md)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

---

## Part Of The Same Toolkit

> Production-grade ExtendScript infrastructure for Illustrator-era JavaScript engines.

<table>
<tr>
<td width="50%" valign="top">

### Runtime Primitives

**[ESON](https://github.com/thelabcorner/eson)**<br>
Strict RFC 8259 JSON for ExtendScript.

**[ESB64](https://github.com/thelabcorner/es-b64)**<br>
Base64 and UTF-8 utilities.

**[ESARR](https://github.com/thelabcorner/es-arr)**<br>
ES5+ Array compatibility methods.

**[ESSTR](https://github.com/thelabcorner/es-str)**<br>
String whitespace and trim methods.

**[ESCHARS](https://github.com/thelabcorner/es-chars)**<br>
Native bulk byte operations.

**[ESHTTP](https://github.com/thelabcorner/es-http)**<br>
HTTP transport for ExtendScript automation.

**[ESTIMER](https://github.com/thelabcorner/es-timer)**<br>
Microsecond timing for ExtendScript automation.

</td>
<td width="50%" valign="top">

### Build & Integration Tools

**[ESPACK](https://github.com/thelabcorner/espack)**<br>
Self-extracting ExternalObject bundles.

**[ESMIN](https://github.com/thelabcorner/es-min)**<br>
Minification for shipped JSX bundles.

**[ESABI](https://github.com/thelabcorner/esabi)**<br>
Modern ExternalObject ABI declarations for native integrations.

**[ESDB](https://github.com/thelabcorner/esdb)**<br>
Native state and durable storage for Adobe tooling.

**[VectorIPC](https://github.com/thelabcorner/vector-ipc)**<br>
Bounded local IPC for scripting hosts and native plug-ins.

**ESOBF** <sub>coming soon</sub><br>
Obfuscation for hardened JSX distribution.

</td>
</tr>
</table>

Also from the same team: **[ArcFit.dev](https://arcfit.dev)**, deterministic arc warp for Illustrator.

---

## Why ESDB?

Adobe-native tools repeatedly need the same infrastructure: durable state, transactions, schema migration, integrity checks, backups, native/ExtendScript interoperability, and a safe path toward reactive application state.

ESDB centralizes that infrastructure without turning every consumer into a key/value database.

The project has two deliberately separate layers:

- **ESDB Runtime** owns SQLite lifecycle, durability policy, transactions, migrations, health, backup, and the storage-backend contract.
- **ESDB Store** is optional. It adds named object stores, canonical values, monotonic revisions, and pull-based change polling.

A native product such as Workmark can use Runtime with its own relational schema. An ExtendScript utility can eventually use Store as a higher-level state API. Both share the same native engine.

ESDB is **not** "SQLite exposed to ExtendScript." SQLite is the durable kernel; ESDB defines the lifecycle, portability, typed values, revision model, and Adobe-facing boundary around it.

## Status

0.1.0 is a foundation build. It currently includes:

- pinned SQLite 3.53.4;
- stable opaque-handle C ABI;
- move-only C++17 RAII facade;
- explicit open/journal/synchronous/cache policies;
- transactions and LIFO savepoints;
- product-owned schema migrations using PRAGMA user_version;
- integrity and online backup APIs;
- backend capability and database health snapshots;
- optional typed Store with durable revisions/change polling;
- ESABI v0.3-compatible ExternalObject adapter;
- minimal ES3-safe JSX facade with explicit unload lifecycle;
- native and adapter smoke/hardening tests;
- live Illustrator 30.6.0 / ExtendScript 4.5.6 facade validation.

Transparent page compression is intentionally **not enabled yet**. See [docs/COMPRESSION.md](docs/COMPRESSION.md).

## Architecture

~~~text
                         ESDB
                          |
              +-----------+-----------+
              |                       |
         ESDB Runtime             ESDB Store
              |                       |
     SQLite ownership          named object stores
     transactions              canonical values
     migrations                durable revisions
     backup/integrity          change polling
     health/backend caps              |
              +-----------+-----------+
                          |
                    ESDB Core C++
                          |
                  stable esdb_* C ABI
                    /             \
                   /               \
          native C/C++          ESABI adapter
             callers                |
                              ExternalObject
                                    |
                              extendscript/esdb.jsx
~~~

Runtime has no hidden writer thread. Store may grow explicit buffered/cache policies later, but Runtime will remain neutral about scheduling.

For the complete design, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Native API

Public headers live under include/esdb/.

~~~c
#include <esdb/esdb.h>

esdb_error error = {0};
esdb_open_options options;
esdb_database *db = NULL;

esdb_open_options_init(&options);
options.journal_mode = ESDB_JOURNAL_WAL;
options.synchronous = ESDB_SYNCHRONOUS_FULL;

if (esdb_open("state.sqlite", &options, &db, &error) != ESDB_OK) {
    /* inspect error.status / phase / SQLite codes / message */
}

esdb_exec(db,
    "CREATE TABLE IF NOT EXISTS settings("
    " key TEXT PRIMARY KEY,"
    " value TEXT NOT NULL"
    ");",
    &error);

esdb_close(db);
~~~

Runtime intentionally supports consumer-owned SQL schemas. esdb_native_handle() is a controlled escape hatch for native consumers that need APIs ESDB does not wrap; its lifetime and threading rules are documented in [docs/ABI.md](docs/ABI.md).

### C++ RAII

~~~cpp
#include <esdb/esdb.hpp>

esdb::Database db;
esdb::Error error;
esdb_open_options options{};

esdb_open_options_init(&options);
options.journal_mode = ESDB_JOURNAL_WAL;

if (esdb::Database::open("state.sqlite", &options, db, &error) != ESDB_OK) {
    // inspect error
}
~~~

The facade is move-only and status-returning; it does not translate ESDB failures into C++ exceptions.

## Store

Store is optional and lives in esdb_store.h.

~~~c
#include <esdb/esdb.h>
#include <esdb/esdb_store.h>

esdb_value *value = NULL;
uint64_t revision = 0;

esdb_value_create_text(ESDB_VALUE_UTF8, "dark", 4, &value, NULL);
esdb_store_put(db, "settings", "theme", value, &revision, NULL);
esdb_value_destroy(value);
~~~

The canonical value domain is NULL, BOOL, INT32, INT64, DOUBLE, UTF8, BYTES, ARRAY, OBJECT.

INT64 and BYTES remain native lossless types. They are not silently coerced into unsafe ExtendScript numbers/strings.

Store revisions are monotonic durable metadata. Deleting/pruning change rows does not reset the current revision, and reopening the database preserves it.

See [docs/STORE.md](docs/STORE.md).

## ExtendScript

The current adapter is deliberately thin.

~~~jsx
#include "esdb.jsx"

ESDB.load("lib:ESDB");

var db = ESDB.open(new File("~/my-plugin-state.esdb"));
$.writeln(db.healthJSON());
$.writeln(db.dataVersion());
db.close();
~~~

The JSX file passes the shared ESTC ES3 static check and live compile-only parse gate. The current build was also exercised end-to-end inside Illustrator 30.6.0 / ExtendScript 4.5.6: absolute-path DLL load, version queries, database open, health/data-version queries, close, zero-handle verification, and unload all passed.

`ESDB.unload()` refuses to unload while adapter database handles remain open. The ExternalObject adapter does not expose arbitrary SQL, asynchronous JSX callbacks, raw INT64 as a JavaScript Number, or arbitrary bytes through the string channel.

## Build

### Fetch/verify SQLite

~~~powershell
pwsh -File tools/fetch-sqlite.ps1
~~~

The script pins SQLite 3.53.4 and verifies the downloaded amalgamation against its recorded SHA3-256 digest before installation.

### Visual Studio preset

~~~powershell
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release
~~~

ESDB_BUILD_EXTERNALOBJECT=ON builds ESDB.dll on Windows and resolves ESABI from an installed package, ESDB_ESABI_SOURCE_DIR, the sibling ../esabi checkout, or finally the pinned v0.3.0 repository.

## Validation

Current automated coverage includes C/C++ smoke, transactions, rollback, savepoints, migration rollback, canonical Store values, revision continuity after full prune and reopen, signed-64 revision bounds, concurrent Store mutation serialization, change-window semantics, subscription polling, C++ callback containment, integrity/backup, backend capabilities, generation-tagged ExternalObject handles, and thread-local adapter staging.

Illustrator 30.6.0 / ExtendScript 4.5.6 additionally passes the live ESTC parser gate and an end-to-end facade/DLL lifecycle smoke. Crash-injection and broader multi-process qualification remain future backend/release evidence. The exact evidence and qualification boundaries are preserved in [docs/RELEASE-VALIDATION.md](docs/RELEASE-VALIDATION.md).

## Compression

Plain SQLite is the reference backend and remains directly inspectable with stock SQLite tools.

Compression will not be selected by familiarity or ratio alone. A candidate backend must pass the same semantics plus crash recovery, multi-process locking, backup, migration, and tooling gates before performance or size is considered.

See [docs/COMPRESSION.md](docs/COMPRESSION.md).

## Repository layout

~~~text
esdb/
├─ include/esdb/          public C/C++ API
├─ src/                   Runtime, Store, canonical values
├─ adapters/externalobject/
├─ extendscript/          ES3 facade
├─ tests/
├─ tools/                 pinned SQLite acquisition
├─ cmake/
└─ docs/
~~~

## Known limitations

- The only shipped storage backend is plain SQLite.
- Transparent compression is not implemented yet.
- The JSX facade is lifecycle/health-only; the full Store API is not exposed through ExternalObject yet.
- Change-log pruning is explicit; v0.1 does not synthesize a revision-gap event for consumers that request history already pruned.
- A database must outlive its transaction/savepoint/subscription handles.
- FULLMUTEX protects SQLite connection calls, but multi-call logical transaction sequences still require application-level serialization.
- Live Illustrator runtime behavior is currently qualified on Illustrator 30.6.0 / ExtendScript 4.5.6 only; other host versions still require their own compatibility evidence.

## License

MIT. See [LICENSE](LICENSE).
