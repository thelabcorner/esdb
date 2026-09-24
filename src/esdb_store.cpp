#include "esdb_internal.hpp"

#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

struct OwnedChange {
    std::uint64_t revision = 0u;
    esdb_change_operation operation = ESDB_CHANGE_PUT;
    esdb_value_type value_type = ESDB_VALUE_NULL;
    std::string store_name;
    std::string key;
};

bool valid_value_type(int type) noexcept {
    return type >= static_cast<int>(ESDB_VALUE_NULL) &&
           type <= static_cast<int>(ESDB_VALUE_OBJECT);
}

bool parse_revision_text(const char *text, std::uint64_t *out) noexcept {
    if (!text || !text[0] || !out) return false;
    if (text[0] == '0' && text[1] != '\0') return false;
    std::uint64_t value = 0u;
    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(text);
         *p;
         ++p) {
        if (*p < static_cast<unsigned char>('0') ||
            *p > static_cast<unsigned char>('9')) {
            return false;
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(*p - static_cast<unsigned char>('0'));
        if (value > (ESDB_REVISION_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

esdb_status read_store_revision_metadata(
    esdb_database *database,
    std::uint64_t *out_revision,
    esdb_error *error) noexcept {
    if (!database || !database->handle || !out_revision) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "database and out revision are required");
    }

    esdb_detail::Statement statement;
    esdb_status status = statement.prepare(
        database,
        "SELECT m.value, COALESCE(("
        " SELECT seq FROM sqlite_sequence WHERE name='__esdb_changes'"
        "),0) "
        "FROM __esdb_meta m WHERE m.key='store_revision';",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = statement.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step != SQLITE_ROW) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "missing ESDB Store revision metadata");
    }

    if (sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "ESDB Store revision metadata has an invalid storage type");
    }
    const char *text = reinterpret_cast<const char *>(
        sqlite3_column_text(statement.get(), 0));
    const sqlite3_int64 sequence = sqlite3_column_int64(statement.get(), 1);
    std::uint64_t revision = 0u;
    if (!parse_revision_text(text, &revision) || sequence < 0 ||
        revision != static_cast<std::uint64_t>(sequence)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "ESDB Store revision metadata is inconsistent");
    }

    *out_revision = revision;
    return ESDB_OK;
}

esdb_status inspect_store_table_count(
    esdb_database *database,
    int *out_count,
    esdb_error *error) noexcept {
    if (!database || !database->handle || !out_count) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "database and out table count are required");
    }

    esdb_detail::Statement tables;
    esdb_status status = tables.prepare(
        database,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='table' AND name IN("
        "'__esdb_meta','__esdb_stores','__esdb_records','__esdb_changes'"
        ");",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = tables.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step != SQLITE_ROW) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_SQLITE, ESDB_PHASE_STORE,
            database->handle, SQLITE_ERROR,
            "failed to inspect ESDB Store schema");
    }

    const int count = sqlite3_column_int(tables.get(), 0);
    if (count < 0 || count > 4) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "ESDB Store schema table count is invalid");
    }
    *out_count = count;
    return ESDB_OK;
}

esdb_status require_store_writable(
    esdb_database *database,
    esdb_error *error) noexcept {
    if (!database || !database->handle) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "database is required");
    }
    const int read_only = sqlite3_db_readonly(database->handle, "main");
    if (read_only < 0) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
            database->handle, SQLITE_ERROR,
            "SQLite main database was not found");
    }
    if (read_only != 0) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_IO, ESDB_PHASE_STORE,
            database->handle, SQLITE_READONLY,
            "ESDB Store mutation requires a writable database");
    }
    return ESDB_OK;
}

esdb_status invalid_revision(
    esdb_database *database,
    esdb_error *error,
    esdb_phase phase,
    const char *message) noexcept {
    return esdb_detail::fail(
        database,
        error,
        ESDB_ERR_INVALID_ARGUMENT,
        phase,
        database ? database->handle : nullptr,
        SQLITE_RANGE,
        message);
}

struct StoreMutationScope {
    bool active = false;
    bool owns_transaction = false;
};

void sync_internal_transaction_state(esdb_database *database) noexcept {
    if (!database || !database->handle) return;
    std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
    database->transaction_active =
        sqlite3_get_autocommit(database->handle) == 0;
    if (!database->transaction_active) database->savepoint_depth = 0u;
}

esdb_status begin_store_mutation(
    esdb_database *database,
    StoreMutationScope *scope,
    esdb_error *error) noexcept {
    if (!database || !database->handle || !scope) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "invalid Store mutation scope");
    }

    scope->active = false;
    scope->owns_transaction = sqlite3_get_autocommit(database->handle) != 0;

    const char *sql = scope->owns_transaction
        ? "BEGIN IMMEDIATE;"
        : "SAVEPOINT __esdb_mutation;";
    const esdb_status status =
        esdb_detail::exec_sql(database, sql, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    scope->active = true;
    if (scope->owns_transaction) sync_internal_transaction_state(database);
    return ESDB_OK;
}

void rollback_store_mutation(
    esdb_database *database,
    StoreMutationScope *scope) noexcept {
    if (!scope || !scope->active) return;

    if (scope->owns_transaction) {
        if (esdb_detail::exec_sql(
                database, "ROLLBACK;", ESDB_PHASE_STORE, nullptr) == ESDB_OK) {
            esdb_detail::count_rollback(database);
        }
        sync_internal_transaction_state(database);
    } else {
        esdb_detail::exec_sql(
            database,
            "ROLLBACK TO __esdb_mutation; RELEASE __esdb_mutation;",
            ESDB_PHASE_STORE,
            nullptr);
    }
    scope->active = false;
}

esdb_status commit_store_mutation(
    esdb_database *database,
    StoreMutationScope *scope,
    esdb_error *error) noexcept {
    if (!scope || !scope->active) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "Store mutation scope is not active");
    }

    const char *sql = scope->owns_transaction
        ? "COMMIT;"
        : "RELEASE __esdb_mutation;";
    const esdb_status status =
        esdb_detail::exec_sql(database, sql, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) {
        rollback_store_mutation(database, scope);
        return status;
    }

    if (scope->owns_transaction) {
        esdb_detail::count_commit(database);
        sync_internal_transaction_state(database);
    }
    scope->active = false;
    return ESDB_OK;
}

esdb_status set_store_revision(
    esdb_database *database,
    sqlite3_int64 revision,
    esdb_error *error) noexcept {
    esdb_detail::Statement statement;
    esdb_status status = statement.prepare(
        database,
        "INSERT INTO __esdb_meta(key,value) VALUES('store_revision',CAST(?1 AS TEXT)) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;

    status = statement.bind_int64(
        database, 1, revision, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = statement.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step != SQLITE_DONE) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_SQLITE, ESDB_PHASE_STORE,
            database->handle, SQLITE_ERROR,
            "store revision update did not finish");
    }
    return ESDB_OK;
}

esdb_status get_store_id(
    esdb_database *database,
    const char *name,
    sqlite3_int64 *out_id,
    bool create,
    esdb_error *error) noexcept {
    if (!out_id) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "out store id is required");
    }

    if (create) {
        esdb_detail::Statement insert;
        esdb_status status = insert.prepare(
            database,
            "INSERT INTO __esdb_stores(name, created_at_ms) VALUES(?1, ?2) "
            "ON CONFLICT(name) DO NOTHING;",
            ESDB_PHASE_STORE,
            error);
        if (status != ESDB_OK) return status;

        status = insert.bind_text(
            database, 1, name, -1, ESDB_PHASE_STORE, error);
        if (status != ESDB_OK) return status;
        status = insert.bind_int64(
            database, 2, esdb_detail::unix_time_ms(),
            ESDB_PHASE_STORE, error);
        if (status != ESDB_OK) return status;

        int step = 0;
        status = insert.step(database, &step, ESDB_PHASE_STORE, error);
        if (status != ESDB_OK) return status;
        if (step != SQLITE_DONE) {
            return esdb_detail::fail(
                database, error, ESDB_ERR_SQLITE, ESDB_PHASE_STORE,
                database->handle, SQLITE_ERROR,
                "store registration did not finish");
        }
    }

    esdb_detail::Statement query;
    esdb_status status = query.prepare(
        database,
        "SELECT id FROM __esdb_stores WHERE name=?1;",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;
    status = query.bind_text(
        database, 1, name, -1, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = query.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step == SQLITE_DONE) return ESDB_ERR_NOT_FOUND;

    *out_id = sqlite3_column_int64(query.get(), 0);
    return ESDB_OK;
}

esdb_status insert_change(
    esdb_database *database,
    sqlite3_int64 store_id,
    const char *key,
    esdb_change_operation operation,
    esdb_value_type value_type,
    sqlite3_int64 changed_at_ms,
    sqlite3_int64 *out_revision,
    esdb_error *error) noexcept {
    if (!out_revision) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "out revision is required");
    }

    esdb_detail::Statement statement;
    esdb_status status = statement.prepare(
        database,
        "INSERT INTO __esdb_changes("
        "store_id,key,operation,value_type,changed_at_ms"
        ") VALUES(?1,?2,?3,?4,?5) RETURNING revision;",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;

    status = statement.bind_int64(
        database, 1, store_id, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    status = statement.bind_text(
        database, 2, key, -1, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    status = statement.bind_int(
        database, 3, static_cast<int>(operation),
        ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    status = statement.bind_int(
        database, 4, static_cast<int>(value_type),
        ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    status = statement.bind_int64(
        database, 5, changed_at_ms, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = statement.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step != SQLITE_ROW) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_SQLITE, ESDB_PHASE_STORE,
            database->handle, SQLITE_ERROR,
            "change insert returned no revision");
    }

    const sqlite3_int64 revision = sqlite3_column_int64(statement.get(), 0);
    if (revision <= 0) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "change insert returned an invalid revision");
    }

    status = statement.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step != SQLITE_DONE) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_SQLITE, ESDB_PHASE_STORE,
            database->handle, SQLITE_ERROR,
            "change insert did not finish");
    }

    *out_revision = revision;
    return ESDB_OK;
}

esdb_status clone_sql_value(
    sqlite3_stmt *statement,
    int type_column,
    int payload_column,
    esdb_value **out_value,
    esdb_database *database,
    esdb_error *error) noexcept {
    const int raw_type = sqlite3_column_int(statement, type_column);
    if (!valid_value_type(raw_type)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "record contains an unknown ESDB value type");
    }

    if (sqlite3_column_type(statement, payload_column) != SQLITE_BLOB) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "record payload is not a canonical BLOB");
    }
    const void *blob = sqlite3_column_blob(statement, payload_column);
    const int bytes = sqlite3_column_bytes(statement, payload_column);
    if (bytes < 0 || (bytes > 0 && !blob)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "record payload is invalid");
    }

    const auto type = static_cast<esdb_value_type>(raw_type);
    bool payload_valid = true;
    switch (type) {
        case ESDB_VALUE_NULL:
            payload_valid = bytes == 0;
            break;
        case ESDB_VALUE_BOOL:
            payload_valid = bytes == 1 && blob &&
                (*static_cast<const std::uint8_t *>(blob) == 0u ||
                 *static_cast<const std::uint8_t *>(blob) == 1u);
            break;
        case ESDB_VALUE_INT32:
            payload_valid = bytes == 4;
            break;
        case ESDB_VALUE_INT64:
        case ESDB_VALUE_DOUBLE:
            payload_valid = bytes == 8;
            break;
        case ESDB_VALUE_UTF8:
        case ESDB_VALUE_ARRAY:
        case ESDB_VALUE_OBJECT:
            payload_valid = esdb_detail::valid_utf8(
                static_cast<const char *>(blob),
                static_cast<std::uint64_t>(bytes));
            break;
        case ESDB_VALUE_BYTES:
            break;
        default:
            payload_valid = false;
            break;
    }
    if (!payload_valid) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "record payload violates its ESDB value type");
    }

    esdb_value *value = new (std::nothrow) esdb_value();
    if (!value) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            database->handle, SQLITE_NOMEM,
            "value allocation failed");
    }

    value->type = type;
    try {
        if (bytes > 0) {
            const auto *begin = static_cast<const std::uint8_t *>(blob);
            value->payload.assign(begin, begin + bytes);
        }
    } catch (...) {
        delete value;
        return esdb_detail::fail(
            database, error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            database->handle, SQLITE_NOMEM,
            "value payload allocation failed");
    }

    *out_value = value;
    return ESDB_OK;
}

}  // namespace

namespace esdb_detail {

esdb_status ensure_store_schema(
    esdb_database *database,
    esdb_error *error) noexcept {
    clear_error(error);
    if (!database || !database->handle) {
        return fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            nullptr, SQLITE_MISUSE, "database is required");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> schema_lock(
        database->store_schema_mutex);
    if (database->store_schema_ready) return ESDB_OK;

    const int read_only = sqlite3_db_readonly(database->handle, "main");
    if (read_only < 0) {
        return fail(
            database, error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
            database->handle, SQLITE_ERROR,
            "SQLite main database was not found");
    }

    esdb_status status = ESDB_OK;
    int store_table_count = 0;
    status = inspect_store_table_count(database, &store_table_count, error);
    if (status != ESDB_OK) return status;

    if (store_table_count != 0 && store_table_count != 4) {
        return fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "ESDB Store schema is only partially present");
    }

    if (store_table_count == 0) {
        if (read_only != 0) {
            return fail(
                database, error, ESDB_ERR_NOT_FOUND, ESDB_PHASE_STORE,
                database->handle, SQLITE_NOTFOUND,
                "ESDB Store schema is not present in the read-only database");
        }

        StoreMutationScope schema_scope{};
        status = begin_store_mutation(database, &schema_scope, error);
        if (status != ESDB_OK) return status;

        /*
         * Re-read after acquiring the write lock. Another process may have
         * initialized the Store between our optimistic inspection and
         * BEGIN IMMEDIATE.
         */
        status = inspect_store_table_count(
            database, &store_table_count, error);
        if (status != ESDB_OK) {
            rollback_store_mutation(database, &schema_scope);
            return status;
        }
        if (store_table_count != 0 && store_table_count != 4) {
            rollback_store_mutation(database, &schema_scope);
            return fail(
                database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
                database->handle, SQLITE_CORRUPT,
                "ESDB Store schema is only partially present");
        }

        if (store_table_count == 0) {
            const char *schema_sql =
                "CREATE TABLE __esdb_meta("
                " key TEXT PRIMARY KEY,"
                " value TEXT NOT NULL"
                ") WITHOUT ROWID;"
                "CREATE TABLE __esdb_stores("
                " id INTEGER PRIMARY KEY,"
                " name TEXT NOT NULL UNIQUE,"
                " created_at_ms INTEGER NOT NULL"
                ");"
                "CREATE TABLE __esdb_records("
                " store_id INTEGER NOT NULL,"
                " key TEXT NOT NULL,"
                " type INTEGER NOT NULL,"
                " value BLOB NOT NULL,"
                " revision INTEGER NOT NULL,"
                " created_at_ms INTEGER NOT NULL,"
                " updated_at_ms INTEGER NOT NULL,"
                " PRIMARY KEY(store_id, key),"
                " FOREIGN KEY(store_id) REFERENCES __esdb_stores(id) ON DELETE CASCADE"
                ") WITHOUT ROWID;"
                "CREATE TABLE __esdb_changes("
                " revision INTEGER PRIMARY KEY AUTOINCREMENT,"
                " store_id INTEGER NOT NULL,"
                " key TEXT NOT NULL,"
                " operation INTEGER NOT NULL,"
                " value_type INTEGER NOT NULL,"
                " changed_at_ms INTEGER NOT NULL,"
                " FOREIGN KEY(store_id) REFERENCES __esdb_stores(id) ON DELETE CASCADE"
                ");"
                "CREATE INDEX __esdb_changes_store_revision "
                " ON __esdb_changes(store_id, revision);"
                "INSERT INTO __esdb_meta(key, value) "
                " VALUES('store_schema_version','1');"
                "INSERT INTO __esdb_meta(key, value) "
                " VALUES('store_revision','0');";

            status = exec_sql(
                database, schema_sql, ESDB_PHASE_STORE, error);
            if (status != ESDB_OK) {
                rollback_store_mutation(database, &schema_scope);
                return status;
            }
        }

        status = commit_store_mutation(
            database, &schema_scope, error);
        if (status != ESDB_OK) return status;
    }

    Statement statement;
    status = statement.prepare(
        database,
        "SELECT value FROM __esdb_meta "
        "WHERE key='store_schema_version';",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = statement.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step != SQLITE_ROW) {
        return fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "missing ESDB Store schema version");
    }

    if (sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
        return fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "ESDB Store schema version has an invalid storage type");
    }
    const auto *version =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
    if (!version || std::strcmp(version, "1") != 0) {
        return fail(
            database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_STORE,
            database->handle, SQLITE_MISMATCH,
            "unsupported ESDB Store schema version");
    }

    std::uint64_t current_revision = 0u;
    status = read_store_revision_metadata(database, &current_revision, error);
    if (status != ESDB_OK) return status;

    database->store_schema_ready = true;
    return ESDB_OK;
}

esdb_status store_changes_since_impl(
    esdb_database *database,
    const char *store_name_or_null,
    std::uint64_t after_revision,
    std::uint32_t limit,
    esdb_change_callback callback,
    void *user_data,
    std::uint64_t *out_last_revision,
    std::uint32_t *out_change_count,
    esdb_error *error) noexcept {
    clear_error(error);
    if (out_last_revision) *out_last_revision = after_revision;
    if (out_change_count) *out_change_count = 0u;

    if (!database || !database->handle || !callback ||
        limit > ESDB_CHANGE_LIMIT_MAX) {
        return fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT,
            ESDB_PHASE_SUBSCRIPTION,
            database ? database->handle : nullptr,
            SQLITE_MISUSE,
            "invalid change query arguments");
    }
    if (after_revision > ESDB_REVISION_MAX) {
        return invalid_revision(
            database, error, ESDB_PHASE_SUBSCRIPTION,
            "after_revision exceeds the ESDB revision domain");
    }
    if (store_name_or_null && !valid_store_name(store_name_or_null)) {
        return fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT,
            ESDB_PHASE_SUBSCRIPTION,
            database->handle, SQLITE_MISMATCH,
            "invalid store name");
    }
    if (limit == 0u) limit = ESDB_CHANGE_LIMIT_MAX;

    std::vector<OwnedChange> snapshot;
    try {
        snapshot.reserve(limit < 256u ? limit : 256u);

        {
            /*
             * A Store writer is a multi-statement SQLite sequence. SQLite's
             * FULLMUTEX protects individual calls, not the invariant between
             * the change row, record row, and revision metadata. Serialize
             * this snapshot against Store writers, then release the mutex
             * before invoking user callbacks.
             */
            std::lock_guard<esdb_detail::NoThrowMutex> store_lock(
                database->store_write_mutex);

            esdb_status status = ensure_store_schema(database, error);
            if (status != ESDB_OK) return status;

            const char *sql =
                store_name_or_null
                    ? "SELECT c.revision,c.operation,c.value_type,s.name,c.key "
                      "FROM __esdb_changes c "
                      "JOIN __esdb_stores s ON s.id=c.store_id "
                      "WHERE c.revision>?1 AND s.name=?2 "
                      "ORDER BY c.revision ASC LIMIT ?3;"
                    : "SELECT c.revision,c.operation,c.value_type,s.name,c.key "
                      "FROM __esdb_changes c "
                      "JOIN __esdb_stores s ON s.id=c.store_id "
                      "WHERE c.revision>?1 "
                      "ORDER BY c.revision ASC LIMIT ?2;";

            Statement statement;
            status = statement.prepare(
                database, sql, ESDB_PHASE_SUBSCRIPTION, error);
            if (status != ESDB_OK) return status;
            status = statement.bind_int64(
                database, 1, static_cast<sqlite3_int64>(after_revision),
                ESDB_PHASE_SUBSCRIPTION, error);
            if (status != ESDB_OK) return status;

            if (store_name_or_null) {
                status = statement.bind_text(
                    database, 2, store_name_or_null, -1,
                    ESDB_PHASE_SUBSCRIPTION, error);
                if (status != ESDB_OK) return status;
                status = statement.bind_int(
                    database, 3, static_cast<int>(limit),
                    ESDB_PHASE_SUBSCRIPTION, error);
            } else {
                status = statement.bind_int(
                    database, 2, static_cast<int>(limit),
                    ESDB_PHASE_SUBSCRIPTION, error);
            }
            if (status != ESDB_OK) return status;

            while (true) {
                int step = 0;
                status = statement.step(
                    database, &step, ESDB_PHASE_SUBSCRIPTION, error);
                if (status != ESDB_OK) return status;
                if (step == SQLITE_DONE) break;

                const sqlite3_int64 raw_revision =
                    sqlite3_column_int64(statement.get(), 0);
                const auto operation = static_cast<esdb_change_operation>(
                    sqlite3_column_int(statement.get(), 1));
                const auto value_type = static_cast<esdb_value_type>(
                    sqlite3_column_int(statement.get(), 2));
                const char *store_name = reinterpret_cast<const char *>(
                    sqlite3_column_text(statement.get(), 3));
                const char *key = reinterpret_cast<const char *>(
                    sqlite3_column_text(statement.get(), 4));

                if (raw_revision <= 0 ||
                    !store_name || !key ||
                    !valid_store_name(store_name) ||
                    !valid_store_key(key) ||
                    !valid_value_type(static_cast<int>(value_type)) ||
                    (operation != ESDB_CHANGE_PUT &&
                     operation != ESDB_CHANGE_DELETE) ||
                    (operation == ESDB_CHANGE_DELETE &&
                     value_type != ESDB_VALUE_NULL)) {
                    return fail(
                        database, error, ESDB_ERR_CORRUPT,
                        ESDB_PHASE_SUBSCRIPTION,
                        database->handle, SQLITE_CORRUPT,
                        "invalid change journal row");
                }

                OwnedChange owned;
                owned.revision = static_cast<std::uint64_t>(raw_revision);
                owned.operation = operation;
                owned.value_type = value_type;
                owned.store_name.assign(store_name);
                owned.key.assign(key);
                snapshot.emplace_back(std::move(owned));
            }
        }
    } catch (const std::bad_alloc &) {
        return fail(
            database, error, ESDB_ERR_OUT_OF_MEMORY,
            ESDB_PHASE_SUBSCRIPTION,
            database->handle, SQLITE_NOMEM,
            "failed to allocate change snapshot");
    } catch (...) {
        return fail(
            database, error, ESDB_ERR_INTERNAL,
            ESDB_PHASE_SUBSCRIPTION,
            database->handle, SQLITE_ERROR,
            "unexpected exception while building change snapshot");
    }

    std::uint64_t last = after_revision;
    std::uint32_t delivered = 0u;
    for (const OwnedChange &owned : snapshot) {
        esdb_change change{};
        change.revision = owned.revision;
        change.operation = owned.operation;
        change.value_type = owned.value_type;
        change.store_name = owned.store_name.c_str();
        change.key = owned.key.c_str();

        last = change.revision;
        ++delivered;
        try {
            if (callback(&change, user_data) != 0) break;
        } catch (...) {
            return fail(
                database, error, ESDB_ERR_INTERNAL,
                ESDB_PHASE_SUBSCRIPTION,
                database->handle, SQLITE_ABORT,
                "change callback threw an exception");
        }
    }

    if (out_last_revision) *out_last_revision = last;
    if (out_change_count) *out_change_count = delivered;
    return ESDB_OK;
}

}  // namespace esdb_detail

esdb_status esdb_store_ensure(
    esdb_database *database,
    const char *store_name_utf8,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle ||
        !esdb_detail::valid_store_name(store_name_utf8)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISMATCH,
            "invalid store ensure arguments");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> write_lock(
        database->store_write_mutex);
    esdb_status status = esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;

    sqlite3_int64 id = 0;
    status = get_store_id(
        database, store_name_utf8, &id, false, error);
    if (status == ESDB_OK) return ESDB_OK;
    if (status != ESDB_ERR_NOT_FOUND) return status;

    status = require_store_writable(database, error);
    if (status != ESDB_OK) return status;
    return get_store_id(
        database, store_name_utf8, &id, true, error);
}

esdb_status esdb_store_put(
    esdb_database *database,
    const char *store_name_utf8,
    const char *key_utf8,
    const esdb_value *value,
    uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_revision) *out_revision = 0u;

    if (!database || !database->handle ||
        !esdb_detail::valid_store_name(store_name_utf8) ||
        !esdb_detail::valid_store_key(key_utf8) ||
        !value ||
        !valid_value_type(static_cast<int>(value->type))) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "invalid store put arguments");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> write_lock(
        database->store_write_mutex);

    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;
    status = require_store_writable(database, error);
    if (status != ESDB_OK) return status;

    StoreMutationScope mutation{};
    status = begin_store_mutation(database, &mutation, error);
    if (status != ESDB_OK) return status;

    sqlite3_int64 store_id = 0;
    status = get_store_id(
        database, store_name_utf8, &store_id, true, error);
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    sqlite3_int64 revision = 0;
    status = insert_change(
        database,
        store_id,
        key_utf8,
        ESDB_CHANGE_PUT,
        value->type,
        esdb_detail::unix_time_ms(),
        &revision,
        error);
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    esdb_detail::Statement record;
    status = record.prepare(
        database,
        "INSERT INTO __esdb_records("
        "store_id,key,type,value,revision,created_at_ms,updated_at_ms"
        ") VALUES(?1,?2,?3,?4,?5,?6,?6) "
        "ON CONFLICT(store_id,key) DO UPDATE SET "
        "type=excluded.type,"
        "value=excluded.value,"
        "revision=excluded.revision,"
        "updated_at_ms=excluded.updated_at_ms;",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    const sqlite3_int64 now = esdb_detail::unix_time_ms();
    status = record.bind_int64(
        database, 1, store_id, ESDB_PHASE_STORE, error);
    if (status == ESDB_OK) {
        status = record.bind_text(
            database, 2, key_utf8, -1, ESDB_PHASE_STORE, error);
    }
    if (status == ESDB_OK) {
        status = record.bind_int(
            database, 3, static_cast<int>(value->type),
            ESDB_PHASE_STORE, error);
    }
    if (status == ESDB_OK) {
        if (value->payload.empty()) {
            status = record.bind_zeroblob(
                database, 4, 0, ESDB_PHASE_STORE, error);
        } else {
            status = record.bind_blob64(
                database, 4, value->payload.data(),
                static_cast<std::uint64_t>(value->payload.size()),
                ESDB_PHASE_STORE, error);
        }
    }
    if (status == ESDB_OK) {
        status = record.bind_int64(
            database, 5, revision, ESDB_PHASE_STORE, error);
    }
    if (status == ESDB_OK) {
        status = record.bind_int64(
            database, 6, now, ESDB_PHASE_STORE, error);
    }
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    int step = 0;
    status = record.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK || step != SQLITE_DONE) {
        if (status == ESDB_OK) {
            status = esdb_detail::fail(
                database, error, ESDB_ERR_SQLITE, ESDB_PHASE_STORE,
                database->handle, SQLITE_ERROR,
                "record upsert did not finish");
        }
        rollback_store_mutation(database, &mutation);
        return status;
    }

    status = set_store_revision(database, revision, error);
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    status = commit_store_mutation(database, &mutation, error);
    if (status != ESDB_OK) return status;

    if (out_revision) {
        *out_revision = static_cast<std::uint64_t>(revision);
    }
    return ESDB_OK;
}

esdb_status esdb_store_get(
    esdb_database *database,
    const char *store_name_utf8,
    const char *key_utf8,
    esdb_value **out_value,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_value) *out_value = nullptr;

    if (!database || !database->handle || !out_value ||
        !esdb_detail::valid_store_name(store_name_utf8) ||
        !esdb_detail::valid_store_key(key_utf8)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "invalid store get arguments");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> store_lock(
        database->store_write_mutex);
    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;

    esdb_detail::Statement statement;
    status = statement.prepare(
        database,
        "SELECT r.type,r.value "
        "FROM __esdb_records r "
        "JOIN __esdb_stores s ON s.id=r.store_id "
        "WHERE s.name=?1 AND r.key=?2;",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;
    status = statement.bind_text(
        database, 1, store_name_utf8, -1, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    status = statement.bind_text(
        database, 2, key_utf8, -1, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = statement.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step == SQLITE_DONE) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_NOT_FOUND, ESDB_PHASE_STORE,
            database->handle, SQLITE_NOTFOUND,
            "store key was not found");
    }

    return clone_sql_value(
        statement.get(), 0, 1, out_value, database, error);
}

esdb_status esdb_store_delete(
    esdb_database *database,
    const char *store_name_utf8,
    const char *key_utf8,
    int *out_deleted,
    uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_deleted) *out_deleted = 0;
    if (out_revision) *out_revision = 0u;

    if (!database || !database->handle ||
        !esdb_detail::valid_store_name(store_name_utf8) ||
        !esdb_detail::valid_store_key(key_utf8)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "invalid store delete arguments");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> write_lock(
        database->store_write_mutex);

    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;

    sqlite3_int64 store_id = 0;
    status = get_store_id(
        database, store_name_utf8, &store_id, false, error);
    if (status == ESDB_ERR_NOT_FOUND) return ESDB_OK;
    if (status != ESDB_OK) return status;
    status = require_store_writable(database, error);
    if (status != ESDB_OK) return status;

    StoreMutationScope mutation{};
    status = begin_store_mutation(database, &mutation, error);
    if (status != ESDB_OK) return status;

    esdb_detail::Statement remove;
    status = remove.prepare(
        database,
        "DELETE FROM __esdb_records "
        "WHERE store_id=?1 AND key=?2 RETURNING 1;",
        ESDB_PHASE_STORE,
        error);
    if (status == ESDB_OK) {
        status = remove.bind_int64(
            database, 1, store_id, ESDB_PHASE_STORE, error);
    }
    if (status == ESDB_OK) {
        status = remove.bind_text(
            database, 2, key_utf8, -1, ESDB_PHASE_STORE, error);
    }
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    int step = 0;
    status = remove.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    const bool deleted = step == SQLITE_ROW;
    if (deleted) {
        status = remove.step(database, &step, ESDB_PHASE_STORE, error);
        if (status != ESDB_OK || step != SQLITE_DONE) {
            if (status == ESDB_OK) {
                status = esdb_detail::fail(
                    database, error, ESDB_ERR_SQLITE, ESDB_PHASE_STORE,
                    database->handle, SQLITE_ERROR,
                    "record delete did not finish");
            }
            rollback_store_mutation(database, &mutation);
            return status;
        }
    }

    if (!deleted) {
        return commit_store_mutation(database, &mutation, error);
    }

    sqlite3_int64 revision = 0;
    status = insert_change(
        database,
        store_id,
        key_utf8,
        ESDB_CHANGE_DELETE,
        ESDB_VALUE_NULL,
        esdb_detail::unix_time_ms(),
        &revision,
        error);
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    status = set_store_revision(database, revision, error);
    if (status != ESDB_OK) {
        rollback_store_mutation(database, &mutation);
        return status;
    }

    status = commit_store_mutation(database, &mutation, error);
    if (status != ESDB_OK) return status;

    if (out_deleted) *out_deleted = 1;
    if (out_revision) {
        *out_revision = static_cast<std::uint64_t>(revision);
    }
    return ESDB_OK;
}

esdb_status esdb_store_exists(
    esdb_database *database,
    const char *store_name_utf8,
    const char *key_utf8,
    int *out_exists,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_exists) *out_exists = 0;

    if (!database || !database->handle || !out_exists ||
        !esdb_detail::valid_store_name(store_name_utf8) ||
        !esdb_detail::valid_store_key(key_utf8)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "invalid store exists arguments");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> store_lock(
        database->store_write_mutex);
    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;

    esdb_detail::Statement statement;
    status = statement.prepare(
        database,
        "SELECT 1 "
        "FROM __esdb_records r "
        "JOIN __esdb_stores s ON s.id=r.store_id "
        "WHERE s.name=?1 AND r.key=?2 LIMIT 1;",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;
    status = statement.bind_text(
        database, 1, store_name_utf8, -1, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    status = statement.bind_text(
        database, 2, key_utf8, -1, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = statement.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    *out_exists = step == SQLITE_ROW ? 1 : 0;
    return ESDB_OK;
}

esdb_status esdb_store_count(
    esdb_database *database,
    const char *store_name_utf8,
    uint64_t *out_count,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_count) *out_count = 0u;

    if (!database || !database->handle || !out_count ||
        !esdb_detail::valid_store_name(store_name_utf8)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "invalid store count arguments");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> store_lock(
        database->store_write_mutex);
    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;

    esdb_detail::Statement statement;
    status = statement.prepare(
        database,
        "SELECT COUNT(*) "
        "FROM __esdb_records r "
        "JOIN __esdb_stores s ON s.id=r.store_id "
        "WHERE s.name=?1;",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;
    status = statement.bind_text(
        database, 1, store_name_utf8, -1, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    int step = 0;
    status = statement.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;
    if (step != SQLITE_ROW) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_SQLITE, ESDB_PHASE_STORE,
            database->handle, SQLITE_ERROR,
            "store count returned no row");
    }

    const sqlite3_int64 count = sqlite3_column_int64(statement.get(), 0);
    if (count < 0) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "store count is negative");
    }
    *out_count = static_cast<std::uint64_t>(count);
    return ESDB_OK;
}

esdb_status esdb_store_revision(
    esdb_database *database,
    uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_revision) *out_revision = 0u;

    if (!database || !database->handle || !out_revision) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "database and out_revision are required");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> store_lock(
        database->store_write_mutex);
    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;

    return read_store_revision_metadata(database, out_revision, error);
}

esdb_status esdb_store_changes_since(
    esdb_database *database,
    const char *store_name_or_null_utf8,
    uint64_t after_revision,
    uint32_t limit,
    esdb_change_callback callback,
    void *user_data,
    uint64_t *out_last_revision,
    uint32_t *out_change_count,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    return esdb_detail::store_changes_since_impl(
        database,
        store_name_or_null_utf8,
        after_revision,
        limit,
        callback,
        user_data,
        out_last_revision,
        out_change_count,
        error);
}

esdb_status esdb_store_prune_changes(
    esdb_database *database,
    uint64_t through_revision,
    uint64_t *out_deleted,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_deleted) *out_deleted = 0u;

    if (!database || !database->handle) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "database is required");
    }
    if (through_revision > ESDB_REVISION_MAX) {
        return invalid_revision(
            database, error, ESDB_PHASE_STORE,
            "through_revision exceeds the ESDB revision domain");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> write_lock(
        database->store_write_mutex);

    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;
    status = require_store_writable(database, error);
    if (status != ESDB_OK) return status;

    esdb_detail::Statement statement;
    status = statement.prepare(
        database,
        "DELETE FROM __esdb_changes WHERE revision<=?1 RETURNING revision;",
        ESDB_PHASE_STORE,
        error);
    if (status != ESDB_OK) return status;
    status = statement.bind_int64(
        database, 1, static_cast<sqlite3_int64>(through_revision),
        ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    std::uint64_t deleted = 0u;
    while (true) {
        int step = 0;
        status = statement.step(database, &step, ESDB_PHASE_STORE, error);
        if (status != ESDB_OK) return status;
        if (step == SQLITE_DONE) break;

        const sqlite3_int64 revision =
            sqlite3_column_int64(statement.get(), 0);
        if (revision <= 0) {
            return esdb_detail::fail(
                database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
                database->handle, SQLITE_CORRUPT,
                "change prune returned an invalid revision");
        }
        ++deleted;
    }

    if (out_deleted) *out_deleted = deleted;
    return ESDB_OK;
}

esdb_status esdb_subscribe(
    esdb_database *database,
    const char *store_name_or_null_utf8,
    uint64_t after_revision,
    esdb_subscription **out_subscription,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_subscription) *out_subscription = nullptr;

    if (!database || !database->handle || !out_subscription ||
        (store_name_or_null_utf8 &&
         !esdb_detail::valid_store_name(store_name_or_null_utf8))) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT,
            ESDB_PHASE_SUBSCRIPTION,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "invalid subscription arguments");
    }
    if (after_revision > ESDB_REVISION_MAX) {
        return invalid_revision(
            database, error, ESDB_PHASE_SUBSCRIPTION,
            "after_revision exceeds the ESDB revision domain");
    }

    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;

    esdb_subscription *subscription =
        new (std::nothrow) esdb_subscription();
    if (!subscription) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_OUT_OF_MEMORY,
            ESDB_PHASE_SUBSCRIPTION,
            database->handle, SQLITE_NOMEM,
            "subscription allocation failed");
    }

    subscription->database = database;
    subscription->all_stores = store_name_or_null_utf8 == nullptr;
    subscription->revision.store(after_revision, std::memory_order_relaxed);
    if (store_name_or_null_utf8) {
        const std::size_t length = std::strlen(store_name_or_null_utf8);
        std::memcpy(
            subscription->store_name, store_name_or_null_utf8, length);
        subscription->store_name[length] = '\0';
    }

    *out_subscription = subscription;
    return ESDB_OK;
}

esdb_status esdb_subscription_poll(
    esdb_subscription *subscription,
    uint32_t limit,
    esdb_change_callback callback,
    void *user_data,
    uint32_t *out_change_count,
    esdb_error *error) {
    if (out_change_count) *out_change_count = 0u;
    if (!subscription || !subscription->database) {
        esdb_detail::set_error(
            error,
            ESDB_ERR_INVALID_ARGUMENT,
            ESDB_PHASE_SUBSCRIPTION,
            nullptr,
            SQLITE_MISUSE,
            "subscription is required");
        return ESDB_ERR_INVALID_ARGUMENT;
    }

    esdb_detail::count_operation(subscription->database);
    std::unique_lock<esdb_detail::NoThrowMutex> poll_lock(
        subscription->poll_mutex, std::try_to_lock);
    if (!poll_lock.owns_lock()) {
        return esdb_detail::fail(
            subscription->database, error, ESDB_ERR_BUSY,
            ESDB_PHASE_SUBSCRIPTION,
            subscription->database->handle, SQLITE_BUSY,
            "subscription is already being polled");
    }
    const std::uint64_t start_revision =
        subscription->revision.load(std::memory_order_relaxed);
    std::uint64_t last = start_revision;
    std::uint32_t count = 0u;
    const char *filter =
        subscription->all_stores ? nullptr : subscription->store_name;

    const esdb_status status =
        esdb_detail::store_changes_since_impl(
            subscription->database,
            filter,
            start_revision,
            limit,
            callback,
            user_data,
            &last,
            &count,
            error);
    if (status == ESDB_OK) {
        subscription->revision.store(last, std::memory_order_relaxed);
    }
    if (out_change_count) *out_change_count = count;
    return status;
}

uint64_t esdb_subscription_revision(
    const esdb_subscription *subscription) {
    return subscription
        ? subscription->revision.load(std::memory_order_relaxed)
        : 0u;
}

void esdb_subscription_destroy(esdb_subscription *subscription) {
    delete subscription;
}
