# ESDB ORM IR v1

Status: **v1 contract, additive vertical slice.** This document defines the
language-neutral intermediate representation consumed by the deterministic
`esdb-schema` compiler (`tools/esdb-schema/`). Frontends (starting with the
Drizzle frontend in `orm/drizzle/`) produce this IR; the compiler validates,
canonicalizes, hashes, generates C++ / ES3 / TS bindings, and packages ordinary
migration SQL emitted by the pinned Drizzle Kit CLI. The compiler does not own
schema-diff authoring.

The IR is not an ORM runtime. It is a declarative schema + named-query
manifest. Generated ORM artifacts do not accept caller-supplied SQL text; the
generated ES3 lane invokes named generated operations. This is an ORM-v1 boundary, not a prohibition on ESDB's separate implemented
raw-SQL Runtime/ESABI escape hatch. That application-query surface is outside
this IR; SQL text is kept separate from typed bound values and caller values
are never interpolated.

## 1. Document shape

```jsonc
{
  "ir_version": "esdb.ir/v1",          // required, exactly this string
  "schema": {
    "name": "user",                     // required, IR identifier (see §3)
    "dialect": "sqlite",                // required, exactly "sqlite" in v1
    "namespace": "esdb.orm.user"        // optional, informational
  },
  "tables": [ /* §4 */ ],
  "queries": [ /* §5 */ ],
  "integrity": {                        // added by `esdb-schema seal`
    "algorithm": "sha256",
    "canonical": "esdb-canonical-json-v1",
    "hash": "<64 lowercase hex chars>"
  },
  "generator": {                        // optional, non-semantic
    "name": "esdb-schema",
    "version": "0.2.0",
    "source": "orm/drizzle/schema/user.schema.ts"
  },
  "annotations": {                      // optional, non-semantic, free-form
    "frontend": "drizzle-orm@0.45.3"
  }
}
```

Rules:

- Unknown top-level fields are rejected. Only `annotations` may carry
  free-form data, and annotations are excluded from the hash.
- `tables` must be a non-empty array.
- `queries` may be absent or empty; a table-only IR is valid.
- **IR v1 contains no JSON numbers.** Every numeric datum is carried as a
  canonical decimal string (see §6). The validator rejects any JSON number
  anywhere in the document. This removes floating-point formatting ambiguity
  from canonicalization entirely.

## 2. Versioning

- `ir_version` is `"esdb.ir/v1"`.
- The major component is a wire contract: a v1 compiler rejects v2 documents
  and vice versa. v1 compilers reject unknown `ir_version` values with
  diagnostic `IR002`.
- Additive optional fields within v1 are permitted only when every v1 consumer
  can ignore them; the current contract is intentionally closed (strict
  unknown-field rejection) so drift is visible instead of silent.

## 3. Identifiers

Two names exist for every table, column, and query parameter:

- `name` — the IR/logical identifier. Grammar: `[A-Za-z_][A-Za-z0-9_]*`,
  length 1..64.
- `sql_name` — the physical SQL identifier. Grammar: same, length 1..64,
  must not be an SQL keyword, and must not begin with `sqlite_` or `__esdb_`
  (case-insensitive). SQL identifiers are case-insensitive for collision
  detection. SQL names are always emitted double-quoted by the compiler; the
  grammar excludes `\"`, backticks, whitespace, punctuation, and control
  characters, so quoting cannot be escaped.

Query names are `[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)?` (for
example `user.findById`) and must be unique across the document.

All identifiers in the document are validated **before** any SQL text is
produced. Logical identifiers are also checked after the deterministic
camel/snake/Pascal transforms used by generated C++/ES3/TS/bridge surfaces;
collisions, target-language reserved words, prototype hazards, and generated
lifecycle-name collisions are rejected. The compiler never silently sanitizes
or repairs identifiers.

## 4. Tables

```jsonc
{
  "name": "user",
  "sql_name": "user",
  "columns": [
    { "name": "id",         "sql_name": "id",         "type": "integer",
      "nullable": false, "primary_key": true,  "autoincrement": false, "unique": false },
    { "name": "name",       "sql_name": "name",       "type": "text",
      "nullable": false, "primary_key": false, "autoincrement": false, "unique": false },
    { "name": "email",      "sql_name": "email",      "type": "text",
      "nullable": false, "primary_key": false, "autoincrement": false, "unique": true  },
    { "name": "createdAt",  "sql_name": "created_at", "type": "integer",
      "nullable": false, "primary_key": false, "autoincrement": false, "unique": false }
  ],
  "primary_key": ["id"],
  "uniques": [["email"]],
  "defaults": {}                      // optional; see below
}
```

### 4.1 Scalar types (v1)

| IR `type` | SQLite | C++ | TS / ES3 |
|---|---|---|---|
| `integer` | `INTEGER` (signed 64-bit) | `std::int64_t` | `number` with safe-integer policy (§7) |
| `text` | `TEXT` | `std::string` (UTF-8) | `string` |
| `real` | `REAL` (IEEE-754 binary64) | `double` | `number` |
| `blob` | `BLOB` | `std::vector<unsigned char>` | uppercase hex `string` (TS and ES3) |

Unsupported Drizzle column types are rejected by the frontend before IR
creation. Adding a type requires a new IR minor revision plus codegen updates.

### 4.2 Column fields

- `nullable` — required boolean.
- `primary_key` — required boolean. At most one table-level PK is supported in
  v1; it must match `primary_key`.
- `autoincrement` — required boolean. Only valid on a single-column integer
  primary key (`IR013` otherwise).
- `unique` — required boolean. Column-level uniqueness.
- `default` — optional object, alternate to `defaults` map; see §4.4.

### 4.3 Table keys

- `primary_key` — array of logical column `name`s, in declaration order. Required and
  non-empty; must agree exactly with the `primary_key` column flags (`IR014`).
- `uniques` — array of arrays of column `name`s. Each unique key must agree
  with the `unique` column flags (`IR015`); column-flagged uniques are
  emitted here as single-element keys. Order is significant and part of the
  hash.

### 4.4 Defaults

A default is a typed literal:

```jsonc
{ "kind": "literal", "type": "integer", "value": "0" }
```

`type` is one of `integer`, `text`, `real`, `blob`, `null`. `value` is always
a string:

- `integer`: canonical decimal, within signed 64-bit range.
- `real`: canonical decimal token (`-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`).
  `NaN` and infinities are not representable.
- `text`: literal UTF-8 text; NUL is rejected. SQL single quotes are doubled at bootstrap/oracle emission.
- `blob`: uppercase hex, even length.
- `null`: `value` is `""`.

Defaults may be used on nullable or non-nullable columns when their type
matches the column. `DEFAULT NULL` is valid only for a nullable column; a
`NOT NULL` column with a null default is rejected (`IR016`).

### 4.5 Foreign keys, CHECKs, and explicit indexes

Tables may additionally declare three deterministic structural collections:

- `foreign_keys` — sorted by constraint `name`. Each entry declares local
  `columns`, a referenced logical `table` + `columns`, and explicit
  `on_update` / `on_delete` actions from `no action`, `restrict`,
  `cascade`, `set null`, or `set default`. Local/reference arity must
  match; the target columns must exactly match the referenced table's primary
  key or a declared unique key; corresponding IR types must match. `SET NULL`
  requires nullable child columns and `SET DEFAULT` requires child defaults.
- `checks` — sorted named constraints with a compile-time `sql` expression.
  The expression is validated as a bounded static SQL fragment, is part of the
  semantic hash, and never contains caller values.
- `indexes` — sorted explicit indexes with `unique`, one or more ordered
  terms, and optional static `where` predicate. Column terms carry
  `asc`/`desc`; expression terms carry a bounded static SQL expression.
  Partial-index predicates and term order/direction are semantic and therefore
  affect the IR and physical-schema hashes.

These fields describe schema semantics for validation/codegen and the
Kit-vs-IR physical differential. **Drizzle Kit remains the shipped DDL/diff
author**; IR `CREATE TABLE` / `CREATE INDEX` emission is an oracle/bootstrap
surface, not an alternative production migration author.

## 5. Queries

Queries are the only way generated callers reach data. Every query is
parameterized; values are bound, never interpolated.

### 5.1 Common fields

- `name` — required, unique (§3).
- `kind` — one of `select`, `insert`, `upsert`, `update`, `delete`.
- `table` — required, must reference a table `name`.
- `cardinality` — `one` (select only), `many` (select), or `changes`
  (insert/upsert/update/delete).

### 5.2 `select`

```jsonc
{
  "name": "user.findById",
  "kind": "select",
  "table": "user",
  "where": [{ "column": "id", "op": "eq", "param": "id" }],
  "order_by": [{ "column": "id", "direction": "asc" }],
  "limit": "limit",          // optional param name
  "offset": "offset",        // optional param name
  "cardinality": "one"
}
```

- `where` is optional for `cardinality: "many"` (list-all) and required,
  non-empty, and keyed for `cardinality: "one"`. Operators: `eq` only in v1.
- `select` projection is always all columns in declaration order.
- `order_by` is optional; columns must exist. v1 only allows `order_by` when
  the select is not `cardinality: "one"`? No — ordering a keyed single-row
  select is harmless and allowed, but must reference existing columns.
- `cardinality: "one"` requires the `where` set to cover a primary key or a
  declared unique key (`IR021`) so "one" is a real guarantee.
- `limit`/`offset` are optional parameter names; when present, both are bound
  as `integer`.
- `joins` is optional. v1 supports deterministic `inner` and `left` joins.
  Each join names a logical table and one or more equi-column `on` predicates.
  Every predicate must connect the newly joined table to a table already in
  query scope, and the two referenced columns must have the same IR type.
  Repeated/self joins and aliases are intentionally not representable in v1.
- Once joins are present, `where` and `order_by` terms may include a
  `table` qualifier naming a table in scope. The emitted select projection
  remains **only the base table row**, fully qualified, so generated row types
  stay deterministic and no hidden object graph is materialized.

### 5.3 `insert`

```jsonc
{
  "name": "user.insert",
  "kind": "insert",
  "table": "user",
  "values": [
    { "column": "id",         "param": "id" },
    { "column": "name",       "param": "name" },
    { "column": "email",      "param": "email" },
    { "column": "createdAt",  "param": "createdAt" }
  ],
  "cardinality": "changes"
}
```

- Every column not nullable and without a default must be provided exactly
  once (`IR019`).
- Columns may be omitted when nullable or defaulted.

### 5.4 `upsert`

`upsert` uses the same required `values` contract as `insert` plus an
`on_conflict` policy:

```jsonc
{
  "name": "user.upsertByEmail",
  "kind": "upsert",
  "table": "user",
  "values": [
    { "column": "id", "param": "id" },
    { "column": "name", "param": "name" },
    { "column": "email", "param": "email" },
    { "column": "createdAt", "param": "createdAt" }
  ],
  "on_conflict": {
    "target": ["email"],
    "action": "update",
    "set": [{ "column": "name", "param": "nextName" }]
  },
  "cardinality": "changes"
}
```

- `target` must exactly match the table primary key or a declared unique key,
  in key order.
- `action` is `nothing` or `update`. `nothing` may not contain `set`;
  `update` requires a non-empty deterministic assignment list.
- Emission is fixed `INSERT ... ON CONFLICT (...) DO NOTHING` or
  `DO UPDATE SET ...`; all values are placeholders and bound parameters.

### 5.5 `update`

```jsonc
{
  "name": "user.updateName",
  "kind": "update",
  "table": "user",
  "set": [{ "column": "name", "param": "name" }],
  "where": [{ "column": "id", "op": "eq", "param": "id" }],
  "cardinality": "changes"
}
```

- `set` is required and non-empty.
- `where` is required and must be keyed (primary key or unique key, `IR020`).
  v1 refuses unkeyed updates by construction.

### 5.6 `delete`

```jsonc
{
  "name": "user.deleteById",
  "kind": "delete",
  "table": "user",
  "where": [{ "column": "id", "op": "eq", "param": "id" }],
  "cardinality": "changes"
}
```

- `where` is required and keyed (`IR020`).

### 5.7 API parameter order vs SQL bind order

The two orders are deliberately separate and are derived, never stored as
positional integers. Public generated APIs use ergonomic **API order**; SQLite
execution uses the exact **bind order** implied by the emitted SQL:

- `select`: API/bind = `where` params, then `limit`, then `offset`.
- `insert`: API/bind = `values` order.
- `upsert`: API/bind = `values` order followed by conflict-update `set`
  order (for `DO NOTHING`, only `values` parameters exist).
- `update`: API = `where` params then `set` params (key-first ergonomics);
  bind = `set` params then `where` params because SQL is `UPDATE ... SET ...
  WHERE ...`. Generated code maps by semantic parameter name and must never
  assume these orders are identical.
- `delete`: API/bind = `where` params.

For multi-table schemas, schema-wide ES3 and bridge operation names include
the table prefix (`<schema><Table><Operation>`); repository-local C++ methods
remain operation suffixes. When a single-table schema name matches its table
name, the existing suffix-only ES3 method names are preserved. The full query
`name` remains canonical in IR and generated TypeScript metadata.

Parameter names must be unique within a query (`IR019`), and each parameter's
type is the referenced column's IR type.

## 6. Canonical JSON and hash contract

`esdb-canonical-json-v1`:

1. **Encoding** — UTF-8, no BOM.
2. **Whitespace** — none between tokens. The sealed file is the canonical
   form of the sealed document with **no trailing newline**.
3. **Object keys** — sorted ascending by Unicode code point (not UTF-16 code
   unit; astral keys sort by scalar value).
4. **Strings** — `"` and `\` escaped as `\"` / `\\`; U+0008/0009/000A/000C/000D
   as `\b`/`\t`/`\n`/`\f`/`\r`; other C0 controls as `\u00xx` with lowercase
   hex. All other code points, including non-ASCII, are emitted raw UTF-8.
   Lone surrogates are rejected (`IR024`).
5. **Numbers** — forbidden; see §1 (`IR022`).
6. **Literals** — `true`, `false`, `null`.
7. **Hash projection** — the document with the top-level keys `integrity`,
   `generator`, and `annotations` removed. The SHA-256 of the projection's
   canonical bytes is the IR hash. Note that `integrity.hash` is therefore
   **not** the SHA-256 of the sealed file: the file contains `integrity`
   itself. The projection hash is the contract; `sha256(file bytes)` is a
   different value by construction.
8. **Embedded form** — `integrity = { algorithm: "sha256", canonical:
   "esdb-canonical-json-v1", hash: "<lowercase hex>" }`. `seal` computes it;
   `hash --check` re-computes it and fails on mismatch.

Consequences (tested in `tests/orm/`):

- Key order, whitespace, and generator/annotation changes do not change the
  hash.
- Any semantic change (table, column, constraint, query, default) changes the
  hash.
- `hashProjection(sealed) === sealed.integrity.hash`; `sha256(sealed bytes)`
  differs and is never used as the IR identity.
- Two independent runs of the compiler produce byte-identical outputs because
  every generated artifact embeds the IR hash.

## 7. int64 policy

SQLite `INTEGER` is signed 64-bit; C++ preserves it exactly. TS/ES3 `number`
does not. v1 policy, enforced at generated binding boundaries:

- **C++**: `std::int64_t` in and out, always exact.
- **TS**: parameters accept `number` (must be a safe integer) or a canonical
  decimal `string` (must be within signed 64-bit range). Reads return `number`
  and throw `RangeError` when the stored value is outside
  ±(2^53 − 1) unless the caller requests exact mode, in which case integer
  fields are returned as canonical decimal strings.
- **ES3**: identical policy to TS. Values cross the ESABI bridge as canonical
  decimal strings, so the wire is lossless even when the script-visible value
  is a `number`.
- **Wire**: every integer parameter and result is a canonical decimal string
  (`-?(0|[1-9][0-9]*)`, `-0` normalized to `0`, bounded to
  `[-9223372036854775808, 9223372036854775807]`).

## 8. Diagnostics

Diagnostics carry a stable code, a JSON path, and a message:

```
IR020  $.queries[4].where  update/delete requires a keyed where clause
```

The compiler exits non-zero on the first failing command; `--json` prints
`{ "ok": false, "diagnostics": [...] }`.

## 9. Out of scope for v1

- Migration **diff authoring** inside ESDB. Incremental SQL history is authored
  by the pinned Drizzle Kit CLI, then deterministically linted/packaged by
  `esdb-schema package-migrations` for `esdb_migrate()`.
- The v1 contract includes structural foreign keys, named CHECK constraints,
  explicit indexes (including ordered terms and partial predicates), composite
  key declarations, compile-time INNER/LEFT joins, and deterministic upsert.
  Generated named-query bindings still intentionally avoid lazy relation loading,
  runtime query construction, aliases/self-joins, and arbitrary dynamic predicates.
- Drizzle private serializers/programmatic snapshot internals. ESDB consumes
  only committed Drizzle Kit CLI output files as opaque migration provenance;
  it never imports `drizzle-kit` runtime internals.
- Caller-supplied SQL inside generated ORM operations. The separate typed
  raw-SQL Runtime/ESABI escape hatch is outside IR v1; it transports SQL text
  separately from typed bound values and never interpolates caller values.
