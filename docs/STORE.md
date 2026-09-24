# ESDB Store

ESDB Store is the optional state/object layer above ESDB Runtime.

## Scope

Store is meant for application state such as settings, small durable objects, caches, and eventually reactive state.

It is not a replacement for product-owned relational schema. Applications with joins, constrained graphs, interval models, or domain-specific indexing should use Runtime directly.

## Namespaces

The public model is:

~~~text
store name + key -> value
~~~

Store names and keys are UTF-8 data, never interpolated SQL identifiers.

All Store-owned SQL objects use the reserved __esdb_ prefix.

## Value domain

The canonical values are:

- ESDB_VALUE_NULL
- ESDB_VALUE_BOOL
- ESDB_VALUE_INT32
- ESDB_VALUE_INT64
- ESDB_VALUE_DOUBLE
- ESDB_VALUE_UTF8
- ESDB_VALUE_BYTES
- ESDB_VALUE_ARRAY
- ESDB_VALUE_OBJECT

Numeric scalar payloads use a canonical little-endian binary representation inside ESDB values before storage. Store reads validate the canonical payload shape again: BOOL must be exactly one byte `0`/`1`, INT32 exactly four bytes, INT64/DOUBLE exactly eight bytes, NULL empty, and structured text valid UTF-8. Semantic payload violations are reported as `ESDB_ERR_CORRUPT` rather than being silently decoded.

UTF-8, arrays, and objects are length-delimited byte payloads. v0.1 validates UTF-8 validity but deliberately does not prescribe a JSON parser.

## Mutation atomicity

A put/delete operation writes its change journal record, record mutation, and durable revision metadata as one SQLite transaction scope. When the connection is in autocommit mode, ESDB starts `BEGIN IMMEDIATE` before the mutation; this acquires the SQLite writer slot through the configured busy handler before any Store state is read or changed, avoiding deferred read-to-write upgrade races under WAL/multi-process writers. When a caller already owns a transaction, ESDB uses an internal savepoint instead and participates in that outer transaction.

Rolling back the outer transaction also rolls back the Store mutation and its revision allocation. A revision returned by put/delete inside that transaction is therefore provisional until the outer commit; a rolled-back revision may later be reused by SQLite.

## Durable revisions

A revision identifies committed Store change order.

The authoritative current revision is stored in `__esdb_meta`, not inferred from the maximum surviving change row. ESDB also cross-checks that metadata against SQLite's `sqlite_sequence` entry for `__esdb_changes`; malformed, non-canonical, or inconsistent revision metadata is treated as corruption.

This is necessary because the change log can be pruned.

Invariant:

~~~text
revision(after prune all) == revision(before prune)
revision(after reopen)     == previous revision
next mutation revision     > previous revision
~~~

Regression tests enforce all three.

## Change queries

esdb_store_changes_since() returns rows with revision > after_revision in ascending order.

limit behavior:

- 1..ESDB_CHANGE_LIMIT_MAX: explicit bound;
- 0: use ESDB_CHANGE_LIMIT_MAX;
- values greater than the maximum: invalid argument.

Callbacks receive borrowed store_name and key pointers valid only for the duration of the callback.

A non-zero callback return stops iteration normally.

A C++ exception thrown by a callback is contained and converted to ESDB_ERR_INTERNAL.

## Subscriptions

A subscription is a pull cursor over the same change journal.

Polling:

1. reads changes after the cursor revision;
2. invokes the caller synchronously;
3. advances the cursor to the last delivered revision on success.

No native worker thread invokes host code. The cursor revision is atomically published. Only one poll may be active per subscription: concurrent or callback-reentrant polls on that same subscription return `ESDB_ERR_BUSY` instead of blocking behind the active callback.

Store operations that depend on the multi-statement record/change/revision invariant serialize against Store writers on the same ESDB connection. A complete put/delete/prune mutation cannot interleave with another Store mutation, and reads such as get/count/revision cannot observe a mutation half-applied. Standalone Store mutations acquire SQLite write ownership with `BEGIN IMMEDIATE`, so the configured busy timeout applies at a deterministic acquisition point; inside a caller-owned transaction they use a reserved internal savepoint. First-use Store schema initialization follows the same rule and rechecks the schema after acquiring write ownership, preventing concurrent processes from racing independent schema creators. Change polling first copies a coherent bounded snapshot while serialized, then releases the Store mutex before invoking callbacks. This does not turn an application-level begin -> many calls -> commit sequence into a cross-thread critical section; callers still serialize logical transaction ownership.

Subscriptions borrow their parent database; destroy them before closing the database.

## Read-only connections

Store reads work through `ESDB_OPEN_READONLY` when the Store schema already exists. Schema discovery on a read-only connection is verification-only: ESDB never attempts `CREATE TABLE` as a side effect of a read.

`esdb_store_ensure()` succeeds for an already-registered store but cannot create a missing store. `put`, mutation-producing `delete`, and change-log pruning require a writable database and fail deterministically on a read-only connection. A read-only database with no ESDB Store schema reports `ESDB_ERR_NOT_FOUND` rather than trying to initialize one.

## Pruning

esdb_store_prune_changes() removes journal rows through a revision without altering Store data or the durable global revision.

v0.1 intentionally leaves pruning policy to the application.

If an application needs guaranteed detection that a cursor fell behind pruned history, it must coordinate pruning with acknowledged consumer revisions. A future Store version may expose an explicit retained-floor/gap contract.

## Future persistence policies

The intended vocabulary is:

- memory: no SQLite persistence;
- buffered: bounded/coalesced persistence with an explicit crash-loss window;
- durable: mutation completion implies committed persistence;
- cache: reconstructible persistence that can be evicted.

Only durable Store behavior exists in v0.1. The other names are architecture direction, not implemented options.
