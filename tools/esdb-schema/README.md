# esdb-schema

Deterministic compiler and migration packager for the ESDB ORM IR v1. The
compiler itself is dependency-free Node ESM. Production schema authoring lives
in the pinned Drizzle frontend; production DDL/diffs come only from the pinned
Drizzle Kit CLI.

```text
node tools/esdb-schema/bin/esdb-schema.mjs <command>
```

## Commands

| Command | Purpose |
|---|---|
| `validate <ir.json> [--json]` | Validate IR v1 and emit stable `IRxxx` diagnostics. |
| `seal --in <ir.json> --out <ir.json>` | Canonicalize the document and embed its semantic-projection SHA-256. |
| `hash <ir.json> [--check]` | Print or verify the semantic-projection hash. |
| `physical-hash --db <database> [--json]` | Read-only canonical hash of SQLite's realized schema. |
| `physical-diff <ir.json> --kit-dir <dir> --kit-version 0.31.11 [--db <database>] [--baseline N] [--json]` | Compare Kit/IR expectations with a realized SQLite schema. |
| `adoption-baseline --db <database> --out <file>` | Write a read-only baseline artifact for an existing database. |
| `adoption-report [<ir.json>] --db <database> ...` | Read-only report comparing the live schema/version with explicit expectations. |
| `generate <ir.json> --out <dir> [--check]` | Generate C++11, ES3, TypeScript, and ESABI bridge model artifacts. |
| `build <ir.json> --out <dir> [--check]` | Alias of `generate`; it does **not** author migration SQL. |
| `package-migrations <ir.json> --kit-dir <dir> --kit-version 0.31.11 --out <dir> [--baseline N] [--ack file] [--check]` | Validate and deterministically package committed Drizzle Kit migration output for `esdb_migrate()`. |
| `bootstrap-migration <ir.json> --out <dir> [--check]` | Emit IR-derived bootstrap DDL for oracle/testing use only. It is not the production migration authority. |

The historical `migrate` command is intentionally retired. Production
migration history must come from `package-migrations`.

Generation and migration packaging require a sealed IR whose embedded hash
matches the canonical semantic projection. `seal` is the only command that
writes the integrity hash. `--check` is read-only: it regenerates in memory
and fails on missing, stale, or unexpected generated artifacts.

## Authority model

The v1 pipeline deliberately keeps responsibilities separate:

```text
Drizzle schema.ts + declared named queries
        |
        |  orm/drizzle (drizzle-orm 0.45.3)
        v
sealed esdb.ir/v1
        |
        +-----------------------> esdb-schema generate
        |                         C++ / ES3 / TS / bridge contract
        |
        |  drizzle-kit 0.31.11 CLI
        v
ordinary Kit migration SQL + snapshots
        |
        |  esdb-schema package-migrations
        v
manifest v2 + normalized SQL + physical-schema provenance + generated C++ migration wrapper
        |
        v
esdb_migrate() -> PRAGMA user_version
```

The Drizzle property/export key is the logical ORM name. The configured SQLite
name is the physical `sql_name`. For example, logical `createdAt` may map to
physical `created_at`; generated TypeScript/ES3 preserve `createdAt`, while
SQL uses `"created_at"`.

Drizzle Kit `0.31.11` is pinned by the packager. Other versions are rejected
rather than silently changing the DDL authority. The packager does not import
Drizzle Kit internals.

## Generated model artifacts

`generate` / `build` emits exactly the model/bridge set:

```text
cpp/<schema>_repository.hpp
es3/<schema>_repository.jsx
ts/<schema>.ts
bridge/<schema>_orm_bridge.h
bridge/<schema>_orm_bridge_stub.c
bridge/<schema>_orm_bridge.json
```

The C++ repository is header-only C++11. It borrows the native `sqlite3*`
from `esdb_native_handle()`, prepares all SQL ahead of execution, validates
prepared-statement parameter/result counts, binds every runtime value, and is
single-caller-at-a-time per repository instance.

For UPDATE operations, public API order and SQLite bind order are deliberately
separate contracts. Public APIs remain key-first (for example
`update_name(id, name)`), while SQL placeholders are bound in emitted SQL
order (`SET name` before `WHERE id`). The TypeScript metadata and bridge
manifest expose both `apiParams` / `api_params` and
`bindParams` / `bind_params`.

The generated ORM ES3 facade never accepts or sends SQL. It invokes only generated
named ESABI operations using strict U1 wire lanes. Separately, ESDB implements a
bounded typed raw-SQL ESABI escape hatch outside generated ORM v1. It keeps SQL
text separate from typed bound values and never interpolates caller values.
Signed 64-bit integers cross the ES3 boundary as canonical decimal strings;
text/blob payloads use byte-exact hex transport.

## Packaged migration artifacts

`package-migrations` emits a separate production migration set:

```text
migrations/v<version>__<slug>.sql
migrations/manifest.json
migrations/<schema>_migrations.hpp
```

The packager normalizes SQL to LF with one trailing newline, sorts migration
sources deterministically, records per-file SHA-256 provenance and snapshot
hashes, and writes manifest format v2. Its SQL policy rejects transaction
control owned by `esdb_migrate()`, Runtime-owned PRAGMAs, ATTACH/DETACH,
VACUUM, temporary-schema objects, and direct SQLite catalog access.

`DROP`, `ALTER`, and `RENAME` are classified as destructive and require an
explicit acknowledgement sidecar before packaging. The generated C++ wrapper
contains the normalized Kit SQL and delegates migration atomicity, sequencing,
rollback, and `user_version` updates to ESDB Runtime.

SQLite-realized physical-schema hashing and IR differential are implemented in
`physical-hash` and `physical-diff`. `adoption-baseline` and `adoption-report`
provide explicit, read-only baseline and live-database checks. The IR hash
identifies compiler semantics; it is not itself a physical-schema hash, and an
adopted nonzero baseline remains unresolved until its external schema is
explicitly supplied and compared.

## Determinism and safety gates

IR v1 validation rejects, among other things, unsafe/colliding generated names,
case-insensitive SQL-name duplicates, the reserved `sqlite_` and
`__esdb_` prefixes, non-canonical int64 literals, NOT NULL + DEFAULT NULL,
NUL text defaults, metadata injection, unsorted tables/queries, and unkeyed
mutations.

Generated output is byte-deterministic: no timestamps, canonical metadata, and
stable lexical ordering. `generate --check` is strict about the generated
model-artifact root; `package-migrations --check` is strict about the
migration artifact root.

## End-to-end example

The checked-in User slice is the executable reference:

```text
examples/orm/user/
  schema/user.schema.ts
  user.queries.json
  drizzle-migrations/
  generated/
  bridge/
  scripts/regenerate.mjs
  scripts/check.mjs
```

From the ESDB root:

```text
npm ci --prefix orm/drizzle
npm ci --prefix examples/orm/user
npm run check --prefix orm/drizzle
npm run check --prefix examples/orm/user
node tests/orm/run.mjs
```

`examples/orm/user/scripts/regenerate.mjs` performs the full authoring ->
seal -> pinned Kit -> package -> model-generation pipeline. See
`examples/orm/user/README.md` for native build/smoke instructions.

The normative IR contract is `orm/ir/esdb-ir-v1.md`.
