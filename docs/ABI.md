# ESDB ABI and lifetime contract

## Stable C surface

The canonical API is C with opaque handles:

- esdb_database
- esdb_transaction
- esdb_value
- esdb_subscription

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

The escape hatch is intended for product-owned prepared statements and SQLite APIs not wrapped by Runtime, not as a reason to bypass ESDB lifecycle policy.

## C++ facade

esdb.hpp is move-only and status-returning.

It does not throw ESDB failures. Callers may layer their own exception policy above returned statuses.

A Database must outlive child Transaction and Savepoint objects. Public savepoint names may not use the case-insensitive `__esdb_` prefix, which is reserved for ESDB's internal transactional scopes.

## ExternalObject adapter

The adapter uses ESABI 0.3.1 as the sole host ABI definition.

Host exports are:

- ESInitialize
- ESGetVersion
- ESFreeMem
- ESTerminate
- ping
- abiVersion
- version
- sqliteVersion
- stage
- openStaged
- close
- health
- lastError
- dataVersion
- handleCount

The adapter is intentionally not an SQL transport.

### Handle safety

JS-visible database handles are positive 32-bit generation-tagged tokens. Reusing a slot advances the generation; a stale token no longer resolves to the new database occupying that slot. Generations never wrap: after the finite generation space for one slot is exhausted, that slot is retired for the remainder of the process so an ancient token cannot become valid again through ABA reuse.

The staged-path channel and adapter last-error state are thread-local. The handle table itself is process-global and mutex-protected.

The JSX facade treats adapter loading transactionally: constructor/ping failures do not publish a bridge or `loadedSpec`, and a partially created ExternalObject is best-effort unloaded. `ESDB.unload()` refuses to run while database handles remain open. Adobe may still keep the underlying DLL image mapped until the Illustrator process exits, so development iterations should use a fresh DLL path/name rather than assuming the file becomes replaceable.

### String ownership

Returned strings are allocated with malloc and released through ESFreeMem, which calls the matching free.

### ES3 wire safety

The ExternalObject string/number surface is not the ESDB native value format.

Rules:

- arbitrary INT64 is not converted to JavaScript Number;
- arbitrary BYTES is not sent as an ExternalObject string;
- large health counters are serialized as decimal JSON strings;
- lifecycle/data-version values that fit the adapter's numeric domain use numeric host values;
- there are no asynchronous callbacks into JSX.

The future Store adapter should use explicit lossless encodings for INT64 and BYTES, and ESON where structured text is appropriate.

## Expected application failures

Expected adapter failures are reflected in normal result values plus lastError(). They are not reported as negative/fatal host runtime errors.

## Validation boundary

Native adapter smoke tests call the exported functions and exercise allocation/free and stale-handle behavior.

Static ESTC parsing, live compile-only parsing, and live facade execution are reported separately. The current v0.1 validation pass exercised the facade and freshly built x64 adapter successfully on Illustrator 30.6.0 / ExtendScript 4.5.6.
