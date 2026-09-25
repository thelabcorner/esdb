# ESDB ORM example: User

This directory is the executable reference slice for the ESDB compiler-oriented
ORM. It demonstrates the complete v1 authority chain with no handwritten DDL
and no runtime SQL transport **inside the generated ORM path** (the generic ESDB
Runtime/ExternalObject typed-SQL escape hatch is a separate surface):

```text
Drizzle schema + named queries
        |
        v
sealed IR
        |
        +--> generated C++ / TypeScript / ES3 / ESABI contract
        |
        v
drizzle-kit 0.31.11 migration history
        |
        v
ESDB migration package -> esdb_migrate() -> PRAGMA user_version
```

## Schema contract

`schema/user.schema.ts` is the single schema authoring source.

| Logical name | SQLite name | SQLite type | C++ member | TS / ES3 |
|---|---|---|---|---|
| `id` | `id` | `INTEGER PRIMARY KEY` | `std::int64_t id` | `number` / exact decimal string |
| `name` | `name` | `TEXT NOT NULL` | `std::string name` | `string` |
| `email` | `email` | `TEXT NOT NULL UNIQUE` | `std::string email` | `string` |
| `createdAt` | `created_at` | `INTEGER NOT NULL` | `std::int64_t created_at` | `createdAt` |

The `createdAt -> created_at` mapping is intentional: Drizzle property keys
are logical ORM names and configured SQLite identifiers are physical names.

The generated named-operation surface is sorted deterministically:
`deleteById`, `findByEmail`, `findById`, `insert`, `list`,
`updateName`.

## What is authoritative

- `schema/user.schema.ts` and `user.queries.json`: human-authored intent.
- `generated/user.ir.json`: canonical sealed semantic contract produced from
  the Drizzle source.
- `drizzle-migrations/`: committed migration history authored by the pinned
  `drizzle-kit@0.31.11` CLI.
- `generated/migrations/`: deterministic ESDB packaging of that Kit history.
- `generated/cpp`, `generated/ts`, `generated/es3`, and
  `generated/bridge`: compiler products; never hand-edit them.

The compiler's IR-derived `bootstrap-migration` exists only as an independent
oracle/testing surface. It is not used as the production DDL authority.

## Regeneration

Install the two pinned Node dependency sets from the ESDB root:

```text
npm ci --prefix orm/drizzle
npm ci --prefix examples/orm/user
```

Then regenerate the slice:

```text
npm run regenerate --prefix examples/orm/user
```

`scripts/regenerate.mjs` performs, in order:

1. Drizzle metadata extraction using `drizzle-orm@0.45.3`.
2. IR canonicalization/sealing.
3. `drizzle-kit@0.31.11 generate` against the committed Kit history.
4. `package-migrations` over the ordinary Kit SQL/snapshots.
5. `generate` for C++/ES3/TS/bridge model artifacts.

The script invokes the locally installed Kit executable; it does not use an
unpinned network-resolved CLI.

## Drift gates

Run:

```text
npm run check --prefix examples/orm/user
node tests/orm/run.mjs
```

The example check re-extracts/seals the IR into a temporary directory and
byte-compares it with the checked-in IR, verifies its integrity hash, runs
strict model-artifact drift checking, and runs strict migration-package drift
checking against the committed Kit history.

The ORM test suite additionally covers canonical hashing, IR hardening,
Drizzle extraction, API-vs-bind ordering, generated C++ syntax, strict ES3 U1
wire behavior, migration linting/acknowledgements, SQL emission, and type
mapping.

## Production migration package

Current generated migration files have this shape:

```text
generated/migrations/
  manifest.json
  user_migrations.hpp
  v0001__clear_mephisto.sql
```

The migration slug is Drizzle Kit output, not an ESDB naming decision.
`manifest.json` is format v2 and records the pinned Kit version, source path,
normalized SQL SHA-256, snapshot SHA-256, destructive classification/
acknowledgement, IR hash, baseline, and target `user_version`.

`user_migrations.hpp` embeds the normalized Kit SQL and delegates execution to
`esdb_migrate()`. ESDB Runtime owns sequencing, the transaction, rollback,
and `PRAGMA user_version`.

## Generated C++ repository

`generated/cpp/user_repository.hpp` is header-only C++11. It borrows the
native connection from `esdb_native_handle()`, prepares named statements once,
and binds every runtime value.

UPDATE has two deliberate orders:

```text
public API:  update_name(id, name)
SQL:         UPDATE "user" SET "name" = ? WHERE "id" = ?
bind order:  name, id
```

The compiler carries API and bind parameter lists independently so ergonomic
key-first public methods cannot accidentally corrupt SQL placeholder ordering.

A repository object is single-caller-at-a-time. SQLite FULLMUTEX serializes
individual SQLite calls, not a complete reset/bind/step sequence.

## Native runtime smoke

A standalone CMake project builds the real ESDB Runtime plus the generated
repository:

```text
cmake -S examples/orm/user -B orm-user-build -DCMAKE_BUILD_TYPE=Release
cmake --build orm-user-build --target esdb_orm_user_smoke --parallel
./orm-user-build/esdb_orm_user_smoke ./orm-user-build/user.sqlite
```

On a multi-config generator, add `--config Release` and run the executable
from its Release directory. Ensure `ESDBCore.dll` is on `PATH` or beside the
executable on Windows.

The smoke is not a syntax-only example. It:

- opens ESDB Runtime;
- applies the generated migration wrapper twice to prove target-version
  idempotence;
- verifies `PRAGMA user_version`;
- verifies repository/migration IR-hash coherence;
- inserts, reads, updates, lists, and deletes through generated prepared
  statements;
- round-trips `INT64_MAX` as a database value.

## Concrete ESABI bridge

`bridge/user_orm_bridge.cpp` is a real project-specific ExternalObject bridge,
not the generated NOT-IMPLEMENTED contract stub.

It implements:

- `ormPing`, `ormVersion`, `ormOpen`, `ormClose`, `ormLastError`;
- every generated User named operation;
- generation-tagged native database handles;
- generated Kit migrations on open plus `user_version` verification;
- strict canonical int64, UTF-8 text, real, blob, nullability, count, and handle
  parsing;
- U1 result/error encoding;
- semantic parameter lookup so API order is not confused with SQL bind order;
- deterministic handle cleanup at `ESTerminate`.

It accepts **no SQL text**.

When the pinned ESABI source is available as the sibling `../esabi` project,
the example CMake project also exposes the real bridge and a host-independent
ESABI bridge smoke:

```text
cmake --build orm-user-build --target user_orm_bridge esdb_orm_user_bridge_smoke --parallel
./orm-user-build/esdb_orm_user_bridge_smoke
```

On Windows this produces `UserOrmBridge.dll`; multi-config generators place
the smoke executable under the selected configuration directory. The bridge
smoke calls the exported ESABI functions directly and validates signature
discovery, ping/version, migration-on-open, CRUD, UPDATE parameter mapping,
strict rejection of a non-canonical integer lane, close/stale-handle rejection,
and reopen/migration idempotence against real ESDB Runtime.

The generated facade also has a real Illustrator live-host gate on Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/live-orm-user-probe.ps1
```

The probe copies `UserOrmBridge.dll` and `ESDBCore.dll` to a uniquely named
temporary side-by-side directory (avoiding Adobe's per-filename native-module
cache), loads the generated ES3 facade through Illustrator's real
`ExternalObject` loader, and exercises CRUD, UPDATE ordering, exact >2^53
integer transport, list decoding, stale-handle rejection, close, and unload.
The validated Illustrator 30.6.0 / ExtendScript 4.5.6 result is recorded in
`docs/RELEASE-VALIDATION.md`.

## ES3 use

The generated facade exposes logical names:

```jsx
#include "generated/es3/user_repository.jsx"

ESDB_ORM_USER.load("lib:UserOrmBridge");
var handle = ESDB_ORM_USER.open(new File("~/user.sqlite"));
var users = ESDB_ORM_USER.repository(handle);

users.insert({
    id: 1,
    name: "Ada",
    email: "ada@example.com",
    createdAt: 1735689600
});

var row = users.findById(1);
users.updateName(1, "Ada King");
users.deleteById(1);

ESDB_ORM_USER.close(handle);
ESDB_ORM_USER.unload();
```

The facade does not construct or transport SQL. It calls generated named ESABI
operations using the strict U1 wire. Integer values outside the ES3 safe range
must be supplied/received as canonical decimal strings in exact-integer mode.

## SQLite native-handle linkage

Generated repositories call the pinned SQLite API against the exact
`sqlite3*` owned by ESDB. On POSIX those SQLite symbols are naturally visible
from the shared ESDB core. On Windows ESDBCore explicitly exports its pinned
SQLite public API so consumers linking `ESDB::esdb` do not load or link a
second SQLite image.

The compiler also provides SQLite-realized physical-schema hashing and
read-only IR/adoption comparisons through `physical-hash`, `physical-diff`,
`adoption-baseline`, and `adoption-report`. The IR hash proves artifact
semantic identity; it is distinct from the live physical-schema identity.
