# ESDB ABI and lifetime contract

## Stable C surface

The canonical API is C with opaque handles:

- esdb_database
- esdb_transaction
- esdb_value
- esdb_object_subscription

Public structs use fixed-width fields and struct_size where forward-compatible extension is expected.

C++ code is an RAII facade over this API; it is not a second implementation.

## Errors

esdb_error is ABI-stable and includes ESDB status/phase plus SQLite primary and extended result codes.

No C++ exception is intended to cross an esdb_* C boundary. Allocation-bearing value operations contain allocation failures, migration callbacks are contained, change callbacks are contained, and native adapter entry points catch exceptions.

## Database lifetime

esdb_close() ends ownership of the database wrapper.

Required ordering:

~~~text
destroy/release subscriptions
destroy/release savepoints
commit/rollback/destroy transaction
close database
~~~

No thread may race esdb_close().

The C++ facade documents and follows the same parent-before-child lifetime rule.

## Connection ownership

Default open flags include ESDB_OPEN_FULLMUTEX.

With FULLMUTEX, SQLite serializes individual connection calls. ESDB's own bookkeeping uses internal mutexes for transaction/savepoint and error state.

FULLMUTEX does not serialize an application's multi-call unit of work. Sharing one connection across threads while one thread owns a logical transaction requires external application-level serialization. ESDB Store adds its own connection-local serialization around complete Store mutations and invariant-sensitive Store reads, but that does not extend to arbitrary Runtime/native-handle SQL.

With NOMUTEX, the caller must serialize all access.

## Controlled SQLite escape hatch

esdb_native_handle() returns the connection's native SQLite handle as an opaque pointer.

The pointer:

- is borrowed;
- is valid only while the esdb_database remains open;
- must not be closed with sqlite3_close*;
- shares the same transaction, locking, pragma, and thread context as ESDB;
- can invalidate ESDB assumptions if the caller mutates reserved __esdb_ tables or changes durability pragmas.

Installed ESDB packages also install the exact pinned SQLite declaration header as:

~~~c
#include <esdb/sqlite3.h>
~~~

A native consumer may cast:

~~~c
sqlite3 *sql = (sqlite3 *)esdb_native_handle(db);
~~~

Consumers must call SQLite through the **same SQLite image that owns the
handle**. Link `ESDB::esdb`; do not separately link another SQLite build.
On POSIX the pinned SQLite public symbols are visible from the ESDB shared
core. On Windows, `ESDBCore.dll` explicitly exports the pinned SQLite public
API so its import library resolves those calls as well. This is validated by
the installed-package C and C++ consumers.

The native-handle escape hatch is intended for product-owned prepared statements and SQLite APIs not wrapped by Runtime, not as a reason to bypass ESDB lifecycle policy.

## Typed SQL query escape hatch

`esdb_query()` is the stable C Runtime's bounded, typed single-statement SQL
surface. It is separate from `esdb_native_handle()`: callers supply SQL text
plus an immutable array of `esdb_value*` parameters, and Runtime performs
prepare/bind/step/finalize on the ESDB-owned SQLite connection.

Contract:

- exactly one SQLite statement is accepted from valid UTF-8 SQL text; a second
  statement/non-whitespace tail is rejected;
- the parameter count must exactly match SQLite's positional placeholders;
- caller values are always bound, never interpolated into SQL text;
- NULL/BOOL/INT32/INT64/DOUBLE/UTF8/BYTES are bound losslessly when SQLite can
  represent the value (NaN DOUBLE parameters are rejected); SQLite INTEGER
  results decode canonically to exact `ESDB_VALUE_INT64`, FLOAT to DOUBLE,
  TEXT to validated UTF8, BLOB to BYTES, and NULL to NULL;
- row values are borrowed only during the synchronous callback;
- a nonzero callback result stops delivery but Runtime continues stepping so
  `out_row_count` remains the true SQLite row count;
- mutating statements report `sqlite3_changes64()`;
- the statement is finalized on success and every failure/exception path, and
  transaction state is reconciled with ESDB afterward.

This is an **application-query** escape hatch, not migration authority.
Production schema DDL/diff remains migration-owned (Drizzle Kit for the ESDB
ORM lane).

## C++ facade

esdb.hpp is move-only and status-returning.

It does not throw ESDB failures. Callers may layer their own exception policy above returned statuses.

A Database must outlive child Transaction and Savepoint objects. Public savepoint names may not use the case-insensitive `__esdb_` prefix, which is reserved for ESDB's internal transactional scopes.

## ExternalObject adapter

The adapter uses ESABI 0.3.1 as the sole host ABI definition and remains a thin transport over the same ESDB Runtime/Store engine. Generated ORM operations stay named/fixed-arity, while the generic ESDB facade also exposes an explicit advanced typed-SQL escape hatch.

The exported direct-method families include:

- lifecycle/identity: `ping`, `abiVersion`, `version`, `sqliteVersion`;
- database handles: `stage`, `stageHex`, `openStaged`, `close`, `handleCount`;
- observability: `health`, `lastError`, `dataVersion`;
- typed SQL: `querySql(handle, sqlHex, parameterPacket, maxRows)`;
- transactions: `transactionBegin`, `transactionCommit`, `transactionRollback`, `transactionActive`;
- durable ObjectStore and process-memory Store operation families.

`querySql` carries SQL UTF-8 bytes (ASCII hex) and a separate typed `P1`
parameter packet. The native adapter decodes both and delegates to
`esdb_query()`; it never performs caller-value interpolation. The reply is a
typed `Q1` packet with column names, returned row count, true total row count,
change count, truncation flag, and typed row values. Adapter limits are
1 MiB SQL, 1,024 parameters, 10,000 returned rows, an 8 MiB parameter packet,
and a 16 MiB encoded result. JSX exposes this as `Database.query()` and
`Database.run()`.

The standard ESABI lifecycle exports (`ESInitialize`, `ESGetVersion`, `ESFreeMem`, `ESTerminate`) are present as well.

### Handle and transaction safety

JS-visible database handles are positive 32-bit generation-tagged tokens. Reusing a slot advances the generation; a stale token no longer resolves to the new database occupying that slot. Generations never wrap: after the finite generation space for one slot is exhausted, that slot is retired for the remainder of the process so an ancient token cannot become valid again through ABA reuse.

Each adapter database slot may own at most one explicit `esdb_transaction*`. Closing a slot or terminating the adapter destroys an active transaction first, which rolls it back if necessary. JSX `Database.transaction()` therefore maps to the canonical native transaction surface instead of emulating atomicity in script.

The staged-path channel and adapter last-error state are thread-local. The handle table is process-global and mutex-protected.

The JSX facade treats adapter loading transactionally: constructor/ping failures do not publish a bridge or `loadedSpec`, and a partially created ExternalObject is best-effort unloaded. `ESDB.unload()` refuses to run while database handles remain open. Adobe may still keep the underlying DLL image mapped until the Illustrator process exits, so development iterations should use a fresh DLL path/name rather than assuming the file becomes replaceable.

### String ownership

Returned strings are allocated with `malloc` and released through `ESFreeMem`, which calls the matching `free`.

### Byte-exact ES3 wire

The measured Adobe ExternalObject string lane is not a byte-transparent representation of arbitrary ExtendScript text: embedded U+0000 can truncate and surrogate/astral code units are not reliable as raw direct strings. ESDB therefore does **not** use raw host strings as its durable Store wire.

The high-level JSX facade encodes these values to ASCII before crossing the ABI:

- database paths through `stageHex`: UTF-8 bytes -> uppercase hex;
- Store names/keys: UTF-8 bytes -> hex;
- UTF8/ARRAY/OBJECT Store values: UTF-8 bytes -> hex;
- BYTES: octets -> hex;
- INT64 and Store revisions/counts: canonical decimal strings;
- DOUBLE: Store's numeric lane is binary64; encoded query/result packets use an
  exact `max_digits10` decimal token, including explicit `Infinity`,
  `-Infinity`, and `-0` handling; a NaN query parameter is rejected rather than
  silently becoming SQLite `NULL`;
- typed SQL parameters use `P1` records and typed SQL results use `Q1` rows;
  SQL text is a separate UTF-8-hex argument and never contains encoded values.

This is deliberately different from the ESDB native value representation. The adapter is a lossless bridge, not the storage format.

`storeGet` uses `V1:<type>:<payload>`. Missing keys use `V1:-1:`. Change windows use an ASCII-only `V1:<lastRevision>:<count>` header followed by revision/operation/value-type/store-hex/key-hex records.

ARRAY/OBJECT parsing is not implemented inside the native adapter. `extendscript/esdb.jsx` accepts an explicit structured codec and auto-detects ESON when it is already present. ESON is a peer integration, not bundled into the MIT-licensed ESDB distribution.

There are no asynchronous callbacks into JSX. Reactive behavior is host-driven polling over the durable change journal.

## Expected application failures

Expected adapter failures are reflected in normal result values plus lastError(). They are not reported as negative/fatal host runtime errors.

## Validation boundary

Native adapter smoke tests call the exported functions and exercise allocation/free and stale-handle behavior.

Static ESTC parsing, live compile-only parsing, and live facade execution are reported separately. The current v0.1 validation pass exercised the facade and freshly built x64 adapter successfully on Illustrator 30.6.0 / ExtendScript 4.5.6.
