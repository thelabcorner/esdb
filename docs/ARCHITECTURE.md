# ESDB architecture

## Principle

ESDB is a reusable Adobe-native application-state and persistence platform whose durable kernel is SQLite.

It is intentionally split into two tiers.

## ESDB Runtime

Runtime is the stable substrate:

- owns one SQLite connection per esdb_database;
- defines open/configuration policy;
- exposes transactions and savepoints;
- provides schema-version/migration helpers without owning consumer schema;
- provides integrity, backup, health, and backend capabilities;
- exposes a controlled native SQLite handle;
- has no hidden writer thread and no background callbacks.

A consumer can use Runtime and never create an ESDB Store table.

### Product-owned schemas

Runtime exists specifically so applications can keep relational models relational. Workmark should keep its version graph, intervals, metadata, and indexes in its own tables inside workmark-helper.

~~~text
Workmark.aip
    |
 VectorIPC
    |
workmark-helper
    |
 Workmark StorageWriter
    |
 ESDB Runtime
    |
 SQLite
~~~

ESDB must not move SQLite into Illustrator's in-process .aip, replace VectorIPC, or turn Workmark records into generic Store blobs.

## ESDB Store

Store is an optional layer above Runtime:

~~~text
(store, key) -> typed value
                  |
               revision
                  |
              change log
~~~

Its job is ergonomic application state and durable object storage, not replacement of relational SQL.

The current Store schema uses reserved __esdb_ tables. Consumers must not create or mutate tables under that prefix.

## Canonical values

The native domain is NULL, BOOL, INT32, INT64, DOUBLE, UTF8, BYTES, ARRAY, OBJECT.

ARRAY and OBJECT are tagged structured-text payloads in v0.1. The tag is part of ESDB's cross-runtime domain; consumers may choose ESON/JSON validation policy above it.

The database format is not an ExtendScript object representation.

## Revisions and reactivity

Each committed Store mutation receives a global monotonically increasing revision.

The current revision is persisted separately from surviving change rows. Therefore:

- pruning all change rows does not reset revision;
- reopening does not reset revision;
- a later mutation receives a greater revision.

Change delivery is pull-based. Native consumers and future JSX Store consumers poll changes since a known revision. ESDB does not call arbitrary ExtendScript from a native thread.

limit == 0 means ESDB_CHANGE_LIMIT_MAX.

Pruning history is explicit. v0.1 does not synthesize a gap event when a caller asks for history already pruned; applications needing that guarantee must coordinate pruning with acknowledged cursors.

## Concurrency

SQLite is compiled serialized (SQLITE_THREADSAFE=1).

ESDB_OPEN_FULLMUTEX is the default and permits SQLite calls on a connection from multiple threads. It does not make a begin -> operations -> commit sequence into an isolated application-level critical section. Shared-connection transaction sequences still require application serialization.

ESDB_OPEN_NOMUTEX requires external serialization of all use of that connection.

ESDB Store additionally serializes complete Store write mutations per connection. This removes connection-global result races and prevents ESDB Store savepoint sequences from interleaving with each other; it does not replace application-level transaction ownership.

No operation may race esdb_close. Database handles must outlive transaction/savepoint/subscription handles.

For cross-process access, behavior is SQLite's behavior for the selected journal/VFS. The plain backend advertises WAL and multi-process support. Future compressed backends must advertise capabilities independently.

## Durability policy

ESDB open options reject journal_mode=OFF and synchronous=OFF.

Native callers may intentionally bypass policy through the SQLite escape hatch, but health reporting reflects the observed configuration.

Store v0.1 mutations are transactional and durable at the surrounding SQLite commit boundary. When no outer transaction exists, a successful mutation releases its outermost savepoint and commits. When an outer transaction exists, returned state/revisions remain provisional until the caller commits it.

Future Store policy vocabulary is reserved for memory, buffered, durable, and cache. Those modes are not exposed before implementations exist.

## Backend contract

esdb_backend_capabilities makes storage assumptions explicit:

- backend ID/version;
- WAL support;
- multi-process support;
- stock-tool compatibility;
- backup/savepoint/URI support;
- compression capability and codec metadata.

v0.1 reports plain SQLite, no compression, and stock-tool compatibility.

## Error model

All native fallible calls return esdb_status. Where accepted, esdb_error adds ESDB status, phase, SQLite primary/extended codes, and bounded diagnostic text.

C++ allocation/callback boundaries used by the C API are contained inside ESDB. Expected errors are statuses, not exceptions.

## Crash/recovery direction

The first correctness oracle is ordinary SQLite.

A compressed backend is not qualified until it passes:

1. clean round-trip parity;
2. transaction atomicity;
3. abrupt process termination during write;
4. WAL/checkpoint/recovery behavior where advertised;
5. concurrent readers/writers and multiple processes where advertised;
6. backup/restore;
7. migration;
8. corruption detection;
9. stock-tool/export behavior;
10. only then throughput and compression ratio.
