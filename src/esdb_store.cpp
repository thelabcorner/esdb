#include "esdb_internal.hpp"

#include <cstring>
#include <new>

namespace {

bool valid_value_type(int type) noexcept {
    return type >= static_cast<int>(ESDB_VALUE_NULL) &&
           type <= static_cast<int>(ESDB_VALUE_OBJECT);
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

void rollback_savepoint(esdb_database *database) noexcept {
    esdb_detail::exec_sql(
        database,
        "ROLLBACK TO __esdb_mutation; RELEASE __esdb_mutation;",
        ESDB_PHASE_STORE,
        nullptr);
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
    sqlite3_int64 committed_at_ms,
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
        "store_id,key,operation,value_type,committed_at_ms"
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
        database, 5, committed_at_ms, ESDB_PHASE_STORE, error);
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

    const void *blob = sqlite3_column_blob(statement, payload_column);
    const int bytes = sqlite3_column_bytes(statement, payload_column);
    if (bytes < 0 || (bytes > 0 && !blob)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_STORE,
            database->handle, SQLITE_CORRUPT,
            "record payload is invalid");
    }

    esdb_value *value = new (std::nothrow) esdb_value();
    if (!value) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            database->handle, SQLITE_NOMEM,
            "value allocation failed");
    }

    value->type = static_cast<esdb_value_type>(raw_type);
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

    std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
    if (database->store_schema_ready) return ESDB_OK;

    const char *schema_sql =
        "SAVEPOINT __esdb_schema_init;"
        "CREATE TABLE IF NOT EXISTS __esdb_meta("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS __esdb_stores("
        " id INTEGER PRIMARY KEY,"
        " name TEXT NOT NULL UNIQUE,"
        " created_at_ms INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS __esdb_records("
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
        "CREATE TABLE IF NOT EXISTS __esdb_changes("
        " revision INTEGER PRIMARY KEY AUTOINCREMENT,"
        " store_id INTEGER NOT NULL,"
        " key TEXT NOT NULL,"
        " operation INTEGER NOT NULL,"
        " value_type INTEGER NOT NULL,"
        " committed_at_ms INTEGER NOT NULL,"
        " FOREIGN KEY(store_id) REFERENCES __esdb_stores(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS __esdb_changes_store_revision "
        " ON __esdb_changes(store_id, revision);"
        "INSERT INTO __esdb_meta(key, value) "
        " VALUES('store_schema_version','1') "
        " ON CONFLICT(key) DO NOTHING;"
        "INSERT INTO __esdb_meta(key, value) "
        " VALUES("
        "   'store_revision',"
        "   CAST(COALESCE(("
        "     SELECT seq FROM sqlite_sequence WHERE name='__esdb_changes'"
        "   ),0) AS TEXT)"
        " ) ON CONFLICT(key) DO NOTHING;"
        "RELEASE __esdb_schema_init;";

    esdb_status status =
        exec_sql(database, schema_sql, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) {
        exec_sql(
            database,
            "ROLLBACK TO __esdb_schema_init; RELEASE __esdb_schema_init;",
            ESDB_PHASE_STORE,
            nullptr);
        return status;
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

    const auto *version =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
    if (!version || std::strcmp(version, "1") != 0) {
        return fail(
            database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_STORE,
            database->handle, SQLITE_MISMATCH,
            "unsupported ESDB Store schema version");
    }

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

    std::uint64_t last = after_revision;
    std::uint32_t count = 0u;

    while (true) {
        int step = 0;
        status = statement.step(
            database, &step, ESDB_PHASE_SUBSCRIPTION, error);
        if (status != ESDB_OK) return status;
        if (step == SQLITE_DONE) break;

        esdb_change change{};
        const sqlite3_int64 raw_revision =
            sqlite3_column_int64(statement.get(), 0);
        if (raw_revision <= 0) {
            return fail(
                database, error, ESDB_ERR_CORRUPT,
                ESDB_PHASE_SUBSCRIPTION,
                database->handle, SQLITE_CORRUPT,
                "change journal contains an invalid revision");
        }

        change.revision = static_cast<std::uint64_t>(raw_revision);
        change.operation = static_cast<esdb_change_operation>(
            sqlite3_column_int(statement.get(), 1));
        change.value_type = static_cast<esdb_value_type>(
            sqlite3_column_int(statement.get(), 2));
        change.store_name = reinterpret_cast<const char *>(
            sqlite3_column_text(statement.get(), 3));
        change.key = reinterpret_cast<const char *>(
            sqlite3_column_text(statement.get(), 4));

        if (!change.store_name || !change.key ||
            !valid_value_type(static_cast<int>(change.value_type)) ||
            (change.operation != ESDB_CHANGE_PUT &&
             change.operation != ESDB_CHANGE_DELETE)) {
            return fail(
                database, error, ESDB_ERR_CORRUPT,
                ESDB_PHASE_SUBSCRIPTION,
                database->handle, SQLITE_CORRUPT,
                "invalid change journal row");
        }

        last = change.revision;
        ++count;
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
    if (out_change_count) *out_change_count = count;
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

    status = esdb_detail::exec_sql(
        database, "SAVEPOINT __esdb_mutation;",
        ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) return status;

    sqlite3_int64 store_id = 0;
    status = get_store_id(
        database, store_name_utf8, &store_id, true, error);
    if (status != ESDB_OK) {
        rollback_savepoint(database);
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
        rollback_savepoint(database);
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
        rollback_savepoint(database);
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
        rollback_savepoint(database);
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
        rollback_savepoint(database);
        return status;
    }

    status = set_store_revision(database, revision, error);
    if (status != ESDB_OK) {
        rollback_savepoint(database);
        return status;
    }

    status = esdb_detail::exec_sql(
        database, "RELEASE __esdb_mutation;",
        ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) {
        rollback_savepoint(database);
        return status;
    }

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

    status = esdb_detail::exec_sql(
        database, "SAVEPOINT __esdb_mutation;",
        ESDB_PHASE_STORE, error);
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
        rollback_savepoint(database);
        return status;
    }

    int step = 0;
    status = remove.step(database, &step, ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) {
        rollback_savepoint(database);
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
            rollback_savepoint(database);
            return status;
        }
    }

    if (!deleted) {
        status = esdb_detail::exec_sql(
            database, "RELEASE __esdb_mutation;",
            ESDB_PHASE_STORE, error);
        if (status != ESDB_OK) rollback_savepoint(database);
        return status;
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
        rollback_savepoint(database);
        return status;
    }

    status = set_store_revision(database, revision, error);
    if (status != ESDB_OK) {
        rollback_savepoint(database);
        return status;
    }

    status = esdb_detail::exec_sql(
        database, "RELEASE __esdb_mutation;",
        ESDB_PHASE_STORE, error);
    if (status != ESDB_OK) {
        rollback_savepoint(database);
        return status;
    }

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

    esdb_status status =
        esdb_detail::ensure_store_schema(database, error);
    if (status != ESDB_OK) return status;

    return esdb_detail::query_u64(
        database,
        "SELECT CAST(value AS INTEGER) "
        "FROM __esdb_meta WHERE key='store_revision';",
        out_revision,
        ESDB_PHASE_STORE,
        error);
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
    subscription->revision = after_revision;
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
    std::uint64_t last = subscription->revision;
    std::uint32_t count = 0u;
    const char *filter =
        subscription->all_stores ? nullptr : subscription->store_name;

    const esdb_status status =
        esdb_detail::store_changes_since_impl(
            subscription->database,
            filter,
            subscription->revision,
            limit,
            callback,
            user_data,
            &last,
            &count,
            error);
    if (status == ESDB_OK) subscription->revision = last;
    if (out_change_count) *out_change_count = count;
    return status;
}

uint64_t esdb_subscription_revision(
    const esdb_subscription *subscription) {
    return subscription ? subscription->revision : 0u;
}

void esdb_subscription_destroy(esdb_subscription *subscription) {
    delete subscription;
}
