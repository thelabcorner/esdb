# ESDB ORM (vertical slice)

Additive, compiler-first ORM layer for ESDB. This directory tree proves the
architecture without introducing an ORM runtime:

```text
Drizzle schema (authoring source)
        |
        v
  orm/drizzle/            frontend: public Drizzle metadata -> IR v1
        |
        v
  orm/ir/                 language-neutral IR v1 spec + canonical hash contract
        |
        v
  tools/esdb-schema/      deterministic compiler: validate, canonicalize, hash,
                          package migrations, generate C++ / ES3 / TS / bridge
        |
        v
  examples/orm/user/      generated User vertical slice + README
        |
        v
  tests/orm/              deterministic + policy tests (no test framework)
```

Nothing here modifies ESDB Runtime, Store, the ExternalObject adapter, CMake,
or the existing docs. The slice is additive and opt-in.

## Contracts

- **IR v1** (`ir/esdb-ir-v1.md`, informative JSON Schema alongside): no JSON
  numbers, strict unknown-field rejection, keyed update/delete, named queries
  only.
- **Canonical hash** (`ir/esdb-ir-v1.md` §6): `integrity.hash` is SHA-256 of
  the canonical *semantic projection* (top-level `integrity`, `generator`, and
  `annotations` excluded). It is deliberately **not** the SHA-256 of the sealed
  file, which contains `integrity` itself.
- **int64 policy** (`ir/esdb-ir-v1.md` §7): C++ uses `std::int64_t` end to end;
  TS/ES3 expose `number` with a safe-integer guard and an exact decimal-string
  mode; the ESABI wire carries canonical decimal strings.
- **ES3 lane**: generated facades call named generated bridge operations only.
  No generated ES3 artifact contains or accepts SQL text, and the project
  bridge contract is a generated ESABI header + reference stub
  (`examples/orm/user/generated/bridge/`).
- **C++ lane**: generated headers consume `esdb_native_handle()` plus the
  pinned SQLite header, using the installed-package contract
  `<esdb/sqlite3.h>` (with `<sqlite3.h>` as the in-tree fallback). All values
  are bound; all statements are prepared.
- **Drizzle isolation**: `drizzle-orm` is imported only by
  `orm/drizzle/src/adapter.ts`, using the public `getTableConfig` /
  `getTableColumns` metadata APIs. `drizzle-kit` is pinned as an upstream CLI
  (0.31.11) and is never imported; migration generation for Drizzle-native
  consumers stays with `drizzle-kit generate`.

## Packages

| Path | Role |
|---|---|
| `ir/` | IR v1 spec and JSON Schema |
| `drizzle/` | Drizzle frontend package (pinned drizzle-orm 0.45.3, drizzle-kit 0.31.11) |
| `../tools/esdb-schema/` | dependency-free Node compiler + code generators |
| `../examples/orm/user/` | runnable User slice: schema, IR, migration, C++/ES3/TS/bridge |
| `../tests/orm/` | `node ../tests/orm/run.mjs` from the ESDB root |

## v1 boundaries

- One `init` migration per schema; incremental migration history is future work.
- Foreign keys, checks, generated columns, boolean/timestamp/json column modes,
  and SQL-expression defaults are rejected by the frontend instead of being
  silently dropped.
- The ES3 lane is single-table; composite keys validate but do not produce ES3
  lookups.
- The generated ES3 facade is not a SQL surface and never will be; new
  capabilities must arrive as new named IR queries.

## Validation status

Run from the ESDB root:

```text
node tests/orm/run.mjs     # 8 modules: deterministic codegen, hash contract,
                           # validation/injection, int64 policy, ES3 wire,
                           # C++ syntax, Drizzle extraction
cd orm/drizzle && npx tsc --noEmit
```

Live Illustrator/ExtendScript validation has **not** been performed for this
slice; the ES3 tests execute the generated facade in `node:vm` against a fake
bridge, which validates decoding/lane logic but not the Adobe ExternalObject
host. The C++ checks are `-fsyntax-only`; running them requires a built ESDB
library.
