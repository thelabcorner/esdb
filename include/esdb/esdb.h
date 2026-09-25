#ifndef ESDB_ESDB_H
#define ESDB_ESDB_H

#include "esdb_types.h"
#include "esdb_value.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESDB Runtime: connection lifecycle, transactions, schema versioning,
 * integrity, backup, and observability over a single native storage engine.
 *
 * The v0.1 engine is stock SQLite 3.53.4, vendored and cryptographically
 * verified from the canonical cmake/sqlite-pin.json release pin, then compiled
 * into the ESDB library. The public header never exposes a sqlite3*
 * type; the controlled escape hatch is esdb_native_handle(), documented in
 * docs/ABI.md.
 *
 * Threading contract (docs/ABI.md "Connection ownership"):
 *   - A database handle may be used from multiple threads only when opened
 *     with ESDB_OPEN_FULLMUTEX (the default in esdb_open_options_init()).
 *   - ESDB_OPEN_NOMUTEX means the caller guarantees one thread at a time.
 *   - No function may race esdb_close(); join or stop all callers first.
 *   - Transaction handles are connection-scoped and single-threaded.
 */

typedef uint32_t esdb_open_flags;
enum {
    ESDB_OPEN_READONLY = 1u << 0,
    ESDB_OPEN_READWRITE = 1u << 1,
    ESDB_OPEN_CREATE = 1u << 2,
    ESDB_OPEN_URI = 1u << 3,
    ESDB_OPEN_FULLMUTEX = 1u << 4,
    ESDB_OPEN_NOMUTEX = 1u << 5,
    ESDB_OPEN_NOFOLLOW = 1u << 6
};

/*
 * Journal modes. esdb_open() rejects MEMORY and OFF because the Runtime open
 * contract is durable by default. Those values remain public so health
 * reporting stays faithful if a native consumer deliberately changes the mode
 * through the controlled SQLite handle.
 */
typedef uint32_t esdb_journal_mode;
enum {
    ESDB_JOURNAL_UNCHANGED = 0u,
    ESDB_JOURNAL_DELETE = 1u,
    ESDB_JOURNAL_TRUNCATE = 2u,
    ESDB_JOURNAL_PERSIST = 3u,
    ESDB_JOURNAL_MEMORY = 4u,
    ESDB_JOURNAL_WAL = 5u,
    ESDB_JOURNAL_OFF = 6u
};

/*
 * Synchronous modes. ESDB_OPEN rejects ESDB_SYNCHRONOUS_OFF with
 * ESDB_ERR_INVALID_ARGUMENT; the value exists so health reporting stays
 * faithful if a consumer changed the mode through the escape hatch.
 */
typedef uint32_t esdb_synchronous_mode;
enum {
    ESDB_SYNCHRONOUS_UNCHANGED = 0u,
    ESDB_SYNCHRONOUS_OFF = 1u,
    ESDB_SYNCHRONOUS_NORMAL = 2u,
    ESDB_SYNCHRONOUS_FULL = 3u,
    ESDB_SYNCHRONOUS_EXTRA = 4u
};

/* Runtime storage selection. Plain SQLite remains the reference backend. */
typedef uint32_t esdb_storage_mode;
enum {
    ESDB_STORAGE_DEFAULT = 0u,     /* current build default; plain in public ESDB */
    ESDB_STORAGE_PLAIN = 1u,       /* require stock SQLite storage */
    ESDB_STORAGE_COMPRESSED = 2u   /* require a compression-capable provider */
};

typedef uint32_t esdb_storage_provider;
enum {
    ESDB_PROVIDER_AUTO = 0u,
    ESDB_PROVIDER_SQLITE = 1u,
    ESDB_PROVIDER_ZIPVFS = 2u
};

typedef uint32_t esdb_codec;
enum {
    ESDB_CODEC_NONE = 0u,
    ESDB_CODEC_ZSTD = 1u,
    ESDB_CODEC_DEFLATE = 2u
};

typedef struct esdb_open_options {
    uint32_t struct_size;
    esdb_open_flags flags;
    uint32_t busy_timeout_ms;
    esdb_journal_mode journal_mode;
    esdb_synchronous_mode synchronous;
    uint32_t cache_kib;
    uint32_t wal_autocheckpoint_pages;
    uint32_t foreign_keys;
    esdb_storage_mode storage_mode;
    esdb_storage_provider storage_provider;
    esdb_codec compression_codec;
    int32_t compression_level;
    uint32_t reserved[2];
} esdb_open_options;

/* Fills `options` with the documented defaults. Never fails. */
ESDB_API void esdb_open_options_init(esdb_open_options *options);

/*
 * Backend capability contract. The fields are fixed-width and ABI-stable.
 * Callers must zero-initialize the struct or set struct_size=sizeof(struct)
 * before calling esdb_backend_capabilities_get().
 *
 * Compression fields describe the selected provider's physical storage. The
 * public build currently advertises plain SQLite only; a compressed request is
 * rejected rather than silently falling back. Licensed ZIPVFS and future open
 * providers plug in beneath this contract.
 */
typedef struct esdb_backend_capabilities {
    uint32_t struct_size;
    char backend_id[ESDB_BACKEND_ID_CAPACITY];
    char backend_version[ESDB_BACKEND_VERSION_CAPACITY];
    uint32_t backend_version_number;
    uint32_t supports_wal;
    uint32_t supports_multiprocess;
    uint32_t supports_stock_tools;
    uint32_t supports_backup;
    uint32_t supports_savepoints;
    uint32_t supports_uri;
    uint32_t compression_supported;
    esdb_codec page_codec;
    uint32_t codec_version;
    char codec_id[ESDB_CODEC_ID_CAPACITY];
    uint32_t reserved[4];
} esdb_backend_capabilities;

ESDB_API esdb_status esdb_backend_capabilities_get(
    esdb_backend_capabilities *out_capabilities,
    esdb_error *error);

/*
 * Database health and metrics snapshot. Zero-initialize the struct or set
 * struct_size=sizeof(struct) before calling esdb_database_health_get().
 * Counters are per-connection and
 * monotonic; they are best-effort under ESDB_OPEN_NOMUTEX (documented) and
 * exact under the default FULLMUTEX configuration.
 */
typedef struct esdb_database_health {
    uint32_t struct_size;
    uint32_t page_size;
    uint64_t page_count;
    uint64_t freelist_count;
    uint64_t database_bytes;
    int64_t configured_cache_kib;
    esdb_journal_mode journal_mode;
    esdb_synchronous_mode synchronous;
    esdb_storage_mode storage_mode;
    esdb_storage_provider storage_provider;
    esdb_codec compression_codec;
    int32_t compression_level;
    uint32_t busy_timeout_ms;
    uint32_t user_version;
    uint32_t in_transaction;
    uint32_t savepoint_depth;
    uint64_t operation_count;
    uint64_t error_count;
    uint64_t busy_count;
    uint64_t commit_count;
    uint64_t rollback_count;
    esdb_status last_error_status;
    esdb_phase last_error_phase;
    int32_t last_error_sqlite_code;
    int32_t last_error_sqlite_extended_code;
    uint32_t reserved[4];
} esdb_database_health;

/*
 * Opens (or creates) a database. `path_utf8` is a UTF-8 filesystem path, or a
 * SQLite URI when ESDB_OPEN_URI is set.
 *
 * On failure *out_database is NULL and `error` (when provided) describes the
 * failure. The returned handle must be released with esdb_close().
 */
ESDB_API esdb_status esdb_open(
    const char *path_utf8,
    const esdb_open_options *options,
    esdb_database **out_database,
    esdb_error *error);

/*
 * Closes a database and frees the handle. All transactions, savepoints, and
 * subscriptions belonging to the connection must be destroyed first; see
 * docs/ABI.md "Handle lifecycle".
 */
ESDB_API void esdb_close(esdb_database *database);

/*
 * Controlled escape hatch: returns the underlying sqlite3* as void*, or NULL.
 *
 * LIFETIME: valid only while the esdb_database handle is open.
 * THREADING: the returned pointer follows the connection's mutex mode; do not
 * use it from a thread that is not allowed to use the esdb_database handle.
 * OWNERSHIP: never close, free, or reparent it. Never hold it past
 * esdb_close().
 */
ESDB_API void *esdb_native_handle(esdb_database *database);

/*
 * Executes one or more semicolon-separated SQL statements. Intended for DDL,
 * migrations, and product-owned schema management. Typed data paths belong to
 * prepared-statement consumers (Store) or the escape hatch; esdb_exec() is
 * deliberately not the only abstraction.
 */
ESDB_API esdb_status esdb_exec(esdb_database *database, const char *sql_utf8, esdb_error *error);

/*
 * Typed single-statement SQL escape hatch.
 *
 * sql_utf8 is prepared as exactly one SQLite statement. Parameters are bound
 * positionally (1..parameter_count) from immutable esdb_value objects; caller
 * values are never interpolated into SQL text. The supplied parameter count
 * must exactly match sqlite3_bind_parameter_count().
 *
 * Row values are borrowed only for the duration of callback. SQLite storage
 * classes map canonically to ESDB values:
 *   NULL -> NULL, INTEGER -> INT64, FLOAT -> DOUBLE,
 *   TEXT -> UTF8 (invalid UTF-8 is rejected), BLOB -> BYTES.
 * A non-zero callback result stops row delivery successfully.
 *
 * out_row_count counts rows produced by SQLite (even after callback asks to stop
 * delivery). out_change_count is zero for read-only
 * statements and sqlite3_changes64() for mutating statements.
 *
 * This is an application-query escape hatch, not a schema-authoring surface:
 * production DDL/diff authority remains with the application's migration
 * system (Drizzle Kit for ESDB ORM).
 */
typedef int (*esdb_query_row_callback)(
    const char *const *column_names,
    const esdb_value *const *values,
    uint32_t column_count,
    void *user_data);

ESDB_API esdb_status esdb_query(
    esdb_database *database,
    const char *sql_utf8,
    const esdb_value *const *parameters,
    uint32_t parameter_count,
    esdb_query_row_callback callback,
    void *user_data,
    uint64_t *out_row_count,
    uint64_t *out_change_count,
    esdb_error *error);

/*
 * PRAGMA data_version: a per-connection counter that changes when another
 * connection commits a change to the database. This is the polling primitive
 * a future reactive Store builds on; it does not change for this connection's
 * own writes (use esdb_object_store_revision() for those).
 */
ESDB_API esdb_status esdb_data_version_get(
    esdb_database *database,
    uint32_t *out_data_version,
    esdb_error *error);

/* ---- transactions ---- */

typedef uint32_t esdb_transaction_mode;
enum {
    ESDB_TRANSACTION_DEFERRED = 0u,
    ESDB_TRANSACTION_IMMEDIATE = 1u,
    ESDB_TRANSACTION_EXCLUSIVE = 2u
};

ESDB_API esdb_status esdb_begin(
    esdb_database *database,
    esdb_transaction_mode mode,
    esdb_transaction **out_transaction,
    esdb_error *error);

ESDB_API esdb_status esdb_commit(esdb_transaction *transaction, esdb_error *error);
ESDB_API esdb_status esdb_rollback(esdb_transaction *transaction, esdb_error *error);

/* Destroys the handle; rolls back first when the transaction is still active. */
ESDB_API void esdb_transaction_destroy(esdb_transaction *transaction);
ESDB_API int esdb_transaction_active(const esdb_transaction *transaction);

/* ---- savepoints ---- */

/*
 * Savepoints are LIFO scoped within an active esdb_transaction. Names use a
 * conservative ASCII token grammar ([A-Za-z0-9_], 1..64 bytes) so they can be
 * quoted safely. The case-insensitive prefix `__esdb_` is reserved for ESDB's
 * internal scopes. release/rollback require the name to match the top of the
 * savepoint stack; out-of-order operations fail with ESDB_ERR_INVALID_STATE.
 *
 * esdb_savepoint_rollback() rolls back to the savepoint and releases it
 * (ROLLBACK TO + RELEASE), matching the common product expectation.
 */
ESDB_API esdb_status esdb_savepoint_begin(
    esdb_database *database,
    const char *name,
    esdb_error *error);
ESDB_API esdb_status esdb_savepoint_release(
    esdb_database *database,
    const char *name,
    esdb_error *error);
ESDB_API esdb_status esdb_savepoint_rollback(
    esdb_database *database,
    const char *name,
    esdb_error *error);
ESDB_API uint32_t esdb_savepoint_depth(const esdb_database *database);

/* ---- schema versioning and migrations ---- */

/*
 * SQLite user_version is ESDB's product schema version slot. esdb_migrate()
 * treats it as the schema version and updates it atomically with each
 * migration step.
 *
 * The callback runs inside ESDB's migration transaction. It may execute DDL or
 * DML through the supplied database, but it must not BEGIN, COMMIT, ROLLBACK,
 * or otherwise take ownership of transaction control (including through the
 * native SQLite escape hatch). Doing so violates the migration atomicity
 * precondition.
 */
ESDB_API esdb_status esdb_user_version_get(
    esdb_database *database,
    uint32_t *out_version,
    esdb_error *error);
ESDB_API esdb_status esdb_user_version_set(
    esdb_database *database,
    uint32_t version,
    esdb_error *error);

/*
 * Applies contiguous single-step migrations from the current user_version up
 * to `target_version` inside one IMMEDIATE transaction. The callback owns the
 * schema work (product-owned DDL); ESDB owns ordering, atomicity, and the
 * version update. A missing step or a failing callback rolls the whole run
 * back, leaving user_version unchanged.
 *
 * Downgrades are rejected with ESDB_ERR_UNSUPPORTED; forward-only is an
 * explicit v0.1 policy.
 */
typedef esdb_status (*esdb_migration_fn)(
    esdb_database *database,
    uint32_t from_version,
    uint32_t to_version,
    void *user_data,
    esdb_error *error);

typedef struct esdb_migration {
    uint32_t from_version;
    uint32_t to_version;
    esdb_migration_fn apply;
    void *user_data;
} esdb_migration;

ESDB_API esdb_status esdb_migrate(
    esdb_database *database,
    uint32_t target_version,
    const esdb_migration *migrations,
    uint32_t migration_count,
    esdb_error *error);

/* ---- integrity, backup, health ---- */

/* quick != 0 runs PRAGMA quick_check, otherwise PRAGMA integrity_check. */
ESDB_API esdb_status esdb_integrity_check(
    esdb_database *database,
    int quick,
    esdb_error *error);

/*
 * Online backup to `target_path_utf8` using sqlite3_backup. The target is
 * created or overwritten. The source database stays open and usable.
 */
ESDB_API esdb_status esdb_backup_to(
    esdb_database *database,
    const char *target_path_utf8,
    esdb_error *error);

ESDB_API esdb_status esdb_database_health_get(
    esdb_database *database,
    esdb_database_health *out_health,
    esdb_error *error);

#ifdef __cplusplus
}
#endif

#endif /* ESDB_ESDB_H */
