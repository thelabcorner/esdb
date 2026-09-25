#include "esdb_internal.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

namespace esdb_detail {

/* ---- error model ---- */

void clear_error(esdb_error *error) noexcept {
    if (!error) return;
    std::memset(error, 0, sizeof(*error));
}

static void copy_message(char *dest, std::size_t capacity, const char *message) noexcept {
    if (!dest || capacity == 0) return;
    if (!message) {
        dest[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(dest, capacity, message, _TRUNCATE);
#else
    std::strncpy(dest, message, capacity - 1);
    dest[capacity - 1] = '\0';
#endif
}

void set_error(
    esdb_error *error,
    esdb_status status,
    esdb_phase phase,
    sqlite3 *db,
    int sqlite_code,
    const char *message) noexcept {
    if (!error) return;
    clear_error(error);
    error->status = status;
    error->phase = phase;
    error->sqlite_code = sqlite_code;
    error->sqlite_extended_code = db ? sqlite3_extended_errcode(db) : sqlite_code;
    const char *resolved = message;
    if ((!resolved || !resolved[0]) && db) resolved = sqlite3_errmsg(db);
    if ((!resolved || !resolved[0]) && sqlite_code != SQLITE_OK) resolved = sqlite3_errstr(sqlite_code);
    copy_message(error->message, sizeof(error->message), resolved ? resolved : "");
}

esdb_status fail(
    esdb_database *database,
    esdb_error *error,
    esdb_status status,
    esdb_phase phase,
    sqlite3 *db,
    int sqlite_code,
    const char *message) noexcept {
    esdb_error recorded{};
    set_error(&recorded, status, phase, db, sqlite_code, message);
    if (error) {
        *error = recorded;
    }
    if (database) {
        database->error_count.fetch_add(1, std::memory_order_relaxed);
        if (status == ESDB_ERR_BUSY) {
            database->busy_count.fetch_add(1, std::memory_order_relaxed);
        }
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->error_mutex);
        database->last_error = recorded;
    }
    return status;
}

esdb_status map_sqlite_status(int code) noexcept {
    switch (code & 0xff) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:
            return ESDB_OK;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return ESDB_ERR_BUSY;
        case SQLITE_NOMEM:
            return ESDB_ERR_OUT_OF_MEMORY;
        case SQLITE_IOERR:
        case SQLITE_CANTOPEN:
        case SQLITE_READONLY:
        case SQLITE_FULL:
        case SQLITE_PERM:
            return ESDB_ERR_IO;
        case SQLITE_CONSTRAINT:
            return ESDB_ERR_CONSTRAINT;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return ESDB_ERR_CORRUPT;
        case SQLITE_NOTFOUND:
            return ESDB_ERR_NOT_FOUND;
        case SQLITE_MISUSE:
            return ESDB_ERR_INVALID_STATE;
        case SQLITE_RANGE:
            return ESDB_ERR_INVALID_ARGUMENT;
        case SQLITE_MISMATCH:
            return ESDB_ERR_TYPE_MISMATCH;
        default:
            return ESDB_ERR_SQLITE;
    }
}

void count_operation(esdb_database *database) noexcept {
    if (database) database->operation_count.fetch_add(1, std::memory_order_relaxed);
}

void count_commit(esdb_database *database) noexcept {
    if (database) database->commit_count.fetch_add(1, std::memory_order_relaxed);
}

void count_rollback(esdb_database *database) noexcept {
    if (database) database->rollback_count.fetch_add(1, std::memory_order_relaxed);
}

/* ---- statement guard ---- */

Statement::~Statement() noexcept {
    reset();
}

void Statement::reset() noexcept {
    if (statement_) {
        sqlite3_finalize(statement_);
        statement_ = nullptr;
    }
}

esdb_status Statement::prepare(esdb_database *database, const char *sql, esdb_phase phase, esdb_error *error) noexcept {
    reset();
    if (!database || !database->handle || !sql) {
        return fail(database, error, ESDB_ERR_INVALID_ARGUMENT, phase,
                    database ? database->handle : nullptr, SQLITE_MISUSE,
                    "invalid database or SQL for prepare");
    }
    const int rc = sqlite3_prepare_v3(database->handle, sql, -1, SQLITE_PREPARE_PERSISTENT, &statement_, nullptr);
    if (rc != SQLITE_OK) {
        statement_ = nullptr;
        return fail(database, error, map_sqlite_status(rc), phase, database->handle, rc, nullptr);
    }
    return ESDB_OK;
}

esdb_status Statement::step(esdb_database *database, int *out_step, esdb_phase phase, esdb_error *error) noexcept {
    if (!statement_ || !out_step) {
        return fail(database, error, ESDB_ERR_INVALID_STATE, phase,
                    database ? database->handle : nullptr, SQLITE_MISUSE,
                    "statement is not prepared");
    }
    const int rc = sqlite3_step(statement_);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
        *out_step = rc;
        return ESDB_OK;
    }
    return fail(database, error, map_sqlite_status(rc), phase, database->handle, rc, nullptr);
}

esdb_status Statement::bind_text(
    esdb_database *database,
    int index,
    const char *text,
    int size,
    esdb_phase phase,
    esdb_error *error) noexcept {
    const int rc = sqlite3_bind_text(statement_, index, text, size, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return fail(database, error, map_sqlite_status(rc), phase, database->handle, rc, nullptr);
    }
    return ESDB_OK;
}

esdb_status Statement::bind_blob(
    esdb_database *database,
    int index,
    const void *data,
    int size,
    esdb_phase phase,
    esdb_error *error) noexcept {
    const int rc = sqlite3_bind_blob(statement_, index, data, size, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return fail(database, error, map_sqlite_status(rc), phase, database->handle, rc, nullptr);
    }
    return ESDB_OK;
}

esdb_status Statement::bind_blob64(
    esdb_database *database,
    int index,
    const void *data,
    std::uint64_t size,
    esdb_phase phase,
    esdb_error *error) noexcept {
    const int rc = sqlite3_bind_blob64(
        statement_, index, data, static_cast<sqlite3_uint64>(size), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return fail(database, error, map_sqlite_status(rc), phase, database->handle, rc, nullptr);
    }
    return ESDB_OK;
}

esdb_status Statement::bind_zeroblob(
    esdb_database *database,
    int index,
    int size,
    esdb_phase phase,
    esdb_error *error) noexcept {
    const int rc = sqlite3_bind_zeroblob(statement_, index, size);
    if (rc != SQLITE_OK) {
        return fail(database, error, map_sqlite_status(rc), phase, database->handle, rc, nullptr);
    }
    return ESDB_OK;
}

esdb_status Statement::bind_int64(
    esdb_database *database,
    int index,
    std::int64_t value,
    esdb_phase phase,
    esdb_error *error) noexcept {
    const int rc = sqlite3_bind_int64(statement_, index, static_cast<sqlite3_int64>(value));
    if (rc != SQLITE_OK) {
        return fail(database, error, map_sqlite_status(rc), phase, database->handle, rc, nullptr);
    }
    return ESDB_OK;
}

esdb_status Statement::bind_int(
    esdb_database *database,
    int index,
    int value,
    esdb_phase phase,
    esdb_error *error) noexcept {
    const int rc = sqlite3_bind_int(statement_, index, value);
    if (rc != SQLITE_OK) {
        return fail(database, error, map_sqlite_status(rc), phase, database->handle, rc, nullptr);
    }
    return ESDB_OK;
}

/* ---- validation ---- */

bool valid_utf8(const char *text, std::uint64_t size) noexcept {
    if (size == 0) return true;
    if (!text) return false;
    const auto *s = reinterpret_cast<const unsigned char *>(text);
    std::uint64_t i = 0;
    while (i < size) {
        const unsigned char c = s[i++];
        if (c <= 0x7f) continue;
        std::uint32_t code = 0;
        std::uint32_t need = 0;
        if (c >= 0xc2 && c <= 0xdf) { code = c & 0x1fu; need = 1; }
        else if (c >= 0xe0 && c <= 0xef) { code = c & 0x0fu; need = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { code = c & 0x07u; need = 3; }
        else return false;
        if (i + need > size) return false;
        for (std::uint32_t j = 0; j < need; ++j) {
            const unsigned char cc = s[i++];
            if ((cc & 0xc0u) != 0x80u) return false;
            code = (code << 6) | (cc & 0x3fu);
        }
        if ((need == 2 && code < 0x800u) || (need == 3 && code < 0x10000u)) return false;
        if (code >= 0xd800u && code <= 0xdfffu) return false;
        if (code > 0x10ffffu) return false;
    }
    return true;
}

bool valid_c_string(const char *text, std::size_t max_bytes) noexcept {
    if (!text || !text[0]) return false;
    std::size_t length = 0;
    while (length <= max_bytes && text[length] != '\0') ++length;
    return length > 0 && length <= max_bytes && valid_utf8(text, static_cast<std::uint64_t>(length));
}

bool valid_object_store_name(const char *text) noexcept {
    return valid_c_string(text, ESDB_OBJECT_STORE_NAME_MAX_BYTES);
}

bool valid_object_store_key(const char *text) noexcept {
    return valid_c_string(text, ESDB_OBJECT_STORE_KEY_MAX_BYTES);
}

bool valid_savepoint_name(const char *text) noexcept {
    if (!text || !text[0]) return false;
    std::size_t length = 0;
    while (text[length] != '\0') {
        const unsigned char c = static_cast<unsigned char>(text[length]);
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_';
        if (!allowed) return false;
        if (++length > ESDB_SAVEPOINT_NAME_MAX_BYTES) return false;
    }
    if (length == 0) return false;

    /*
     * SQLite identifiers are case-insensitive. Reserve __esdb_* so public
     * savepoints cannot collide with Store's internal transactional scopes.
     */
    static const char reserved[] = "__esdb_";
    if (length >= sizeof(reserved) - 1u) {
        bool matches = true;
        for (std::size_t i = 0; i < sizeof(reserved) - 1u; ++i) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
            if (c != static_cast<unsigned char>(reserved[i])) {
                matches = false;
                break;
            }
        }
        if (matches) return false;
    }
    return true;
}

std::int64_t unix_time_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

/* ---- SQL helpers ---- */

esdb_status exec_sql(esdb_database *database, const char *sql, esdb_phase phase, esdb_error *error) noexcept {
    if (!database || !database->handle || !sql) {
        return fail(database, error, ESDB_ERR_INVALID_ARGUMENT, phase,
                    database ? database->handle : nullptr, SQLITE_MISUSE, "invalid database or SQL");
    }
    char *message = nullptr;
    const int rc = sqlite3_exec(database->handle, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const esdb_status status = map_sqlite_status(rc);
        fail(database, error, status, phase, database->handle, rc, message);
        sqlite3_free(message);
        return status;
    }
    sqlite3_free(message);
    return ESDB_OK;
}

static esdb_status query_integer64(
    esdb_database *database,
    const char *sql,
    sqlite3_int64 *out,
    esdb_phase phase,
    esdb_error *error) noexcept {
    if (!database || !database->handle || !sql || !out) {
        return fail(database, error, ESDB_ERR_INVALID_ARGUMENT, phase,
                    database ? database->handle : nullptr, SQLITE_MISUSE,
                    "invalid scalar query arguments");
    }
    Statement statement;
    esdb_status status = statement.prepare(database, sql, phase, error);
    if (status != ESDB_OK) return status;
    int step = 0;
    status = statement.step(database, &step, phase, error);
    if (status != ESDB_OK) return status;
    if (step != SQLITE_ROW) {
        return fail(database, error, ESDB_ERR_SQLITE, phase, database->handle, SQLITE_ERROR,
                    "scalar query returned no row");
    }
    *out = sqlite3_column_int64(statement.get(), 0);
    return ESDB_OK;
}

esdb_status query_u64(esdb_database *database, const char *sql, std::uint64_t *out, esdb_phase phase, esdb_error *error) noexcept {
    if (!out) {
        return fail(database, error, ESDB_ERR_INVALID_ARGUMENT, phase,
                    database ? database->handle : nullptr, SQLITE_MISUSE, "out value is required");
    }
    sqlite3_int64 value = 0;
    const esdb_status status = query_integer64(database, sql, &value, phase, error);
    if (status != ESDB_OK) return status;
    if (value < 0) {
        return fail(database, error, ESDB_ERR_SQLITE, phase,
                    database ? database->handle : nullptr, SQLITE_RANGE,
                    "negative value where unsigned scalar was expected");
    }
    *out = static_cast<std::uint64_t>(value);
    return ESDB_OK;
}

esdb_status query_i64(esdb_database *database, const char *sql, std::int64_t *out, esdb_phase phase, esdb_error *error) noexcept {
    if (!out) {
        return fail(database, error, ESDB_ERR_INVALID_ARGUMENT, phase,
                    database ? database->handle : nullptr, SQLITE_MISUSE, "out value is required");
    }
    sqlite3_int64 value = 0;
    const esdb_status status = query_integer64(database, sql, &value, phase, error);
    if (status == ESDB_OK) *out = static_cast<std::int64_t>(value);
    return status;
}

}  // namespace esdb_detail

/* ---- public version surface ---- */

uint32_t esdb_abi_version(void) { return ESDB_ABI_VERSION; }
const char *esdb_version(void) { return ESDB_VERSION_STRING; }
const char *esdb_sqlite_version(void) { return sqlite3_libversion(); }

const char *esdb_status_name(esdb_status status) {
    switch (status) {
        case ESDB_OK: return "ok";
        case ESDB_ERR_INVALID_ARGUMENT: return "invalid_argument";
        case ESDB_ERR_OUT_OF_MEMORY: return "out_of_memory";
        case ESDB_ERR_IO: return "io";
        case ESDB_ERR_SQLITE: return "sqlite";
        case ESDB_ERR_BUSY: return "busy";
        case ESDB_ERR_NOT_FOUND: return "not_found";
        case ESDB_ERR_TYPE_MISMATCH: return "type_mismatch";
        case ESDB_ERR_BUFFER_TOO_SMALL: return "buffer_too_small";
        case ESDB_ERR_CONSTRAINT: return "constraint";
        case ESDB_ERR_CORRUPT: return "corrupt";
        case ESDB_ERR_UNSUPPORTED: return "unsupported";
        case ESDB_ERR_INVALID_STATE: return "invalid_state";
        case ESDB_ERR_MIGRATION: return "migration";
        case ESDB_ERR_INTERNAL: return "internal";
        case ESDB_ERR_GAP: return "gap";
        default: return "unknown";
    }
}

const char *esdb_phase_name(esdb_phase phase) {
    switch (phase) {
        case ESDB_PHASE_NONE: return "none";
        case ESDB_PHASE_OPEN: return "open";
        case ESDB_PHASE_CONFIGURE: return "configure";
        case ESDB_PHASE_EXEC: return "exec";
        case ESDB_PHASE_TRANSACTION: return "transaction";
        case ESDB_PHASE_SAVEPOINT: return "savepoint";
        case ESDB_PHASE_MIGRATION: return "migration";
        case ESDB_PHASE_INTEGRITY: return "integrity";
        case ESDB_PHASE_BACKUP: return "backup";
        case ESDB_PHASE_VALUE: return "value";
        case ESDB_PHASE_STORE: return "store";
        case ESDB_PHASE_SUBSCRIPTION: return "subscription";
        case ESDB_PHASE_HEALTH: return "health";
        case ESDB_PHASE_OBJECT_STORE: return "object_store";
        default: return "unknown";
    }
}

void esdb_error_clear(esdb_error *error) { esdb_detail::clear_error(error); }

void esdb_open_options_init(esdb_open_options *options) {
    if (!options) return;
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->flags = ESDB_OPEN_READWRITE | ESDB_OPEN_CREATE | ESDB_OPEN_FULLMUTEX;
    options->busy_timeout_ms = 5000u;
    options->journal_mode = ESDB_JOURNAL_UNCHANGED;
    options->synchronous = ESDB_SYNCHRONOUS_UNCHANGED;
    options->cache_kib = 0u;
    options->wal_autocheckpoint_pages = 0u;
    options->foreign_keys = 1u;
    options->storage_mode = ESDB_STORAGE_DEFAULT;
    options->storage_provider = ESDB_PROVIDER_AUTO;
    options->compression_codec = ESDB_CODEC_NONE;
    options->compression_level = 0;
}

/* ---- backend capabilities ---- */

esdb_status esdb_backend_capabilities_get(esdb_backend_capabilities *out_capabilities, esdb_error *error) {
    esdb_detail::clear_error(error);
    if (!out_capabilities) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_NONE, nullptr, SQLITE_MISUSE,
                               "out_capabilities is required");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    if (out_capabilities->struct_size != 0u &&
        out_capabilities->struct_size != sizeof(*out_capabilities)) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_NONE, nullptr, SQLITE_MISUSE,
                               "capabilities struct_size mismatch");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    std::memset(out_capabilities, 0, sizeof(*out_capabilities));
    out_capabilities->struct_size = sizeof(*out_capabilities);
    const bool has_zipvfs = esdb_detail::zipvfs_compiled();
    const bool has_zstd = esdb_detail::zipvfs_zstd_compiled();
    std::snprintf(
        out_capabilities->backend_id,
        sizeof(out_capabilities->backend_id),
        "%s",
        has_zipvfs ? "sqlite+zipvfs" : "sqlite");
    std::snprintf(out_capabilities->backend_version, sizeof(out_capabilities->backend_version), "%s", sqlite3_libversion());
    out_capabilities->backend_version_number = static_cast<uint32_t>(sqlite3_libversion_number());
    out_capabilities->supports_wal = 1u;
    out_capabilities->supports_multiprocess = 1u;
    out_capabilities->supports_stock_tools = 1u;
    out_capabilities->supports_backup = 1u;
    out_capabilities->supports_savepoints = 1u;
    out_capabilities->supports_uri = 1u;
    out_capabilities->compression_supported = has_zipvfs ? 1u : 0u;
    out_capabilities->page_codec = ESDB_CODEC_NONE;
    out_capabilities->codec_version = 0u;
    std::snprintf(
        out_capabilities->codec_id,
        sizeof(out_capabilities->codec_id),
        "%s",
        has_zipvfs
            ? (has_zstd ? "deflate,zstd" : "deflate")
            : "none");
    return ESDB_OK;
}

/* ---- open / configure / close ---- */

static int sqlite_open_flags(esdb_open_flags flags) {
    int result = 0;
    if (flags & ESDB_OPEN_READONLY) result |= SQLITE_OPEN_READONLY;
    if (flags & ESDB_OPEN_READWRITE) result |= SQLITE_OPEN_READWRITE;
    if (flags & ESDB_OPEN_CREATE) result |= SQLITE_OPEN_CREATE;
    if (flags & ESDB_OPEN_URI) result |= SQLITE_OPEN_URI;
    if (flags & ESDB_OPEN_FULLMUTEX) result |= SQLITE_OPEN_FULLMUTEX;
    if (flags & ESDB_OPEN_NOMUTEX) result |= SQLITE_OPEN_NOMUTEX;
    if (flags & ESDB_OPEN_NOFOLLOW) result |= SQLITE_OPEN_NOFOLLOW;
    return result;
}

static bool valid_open_flags(esdb_open_flags flags) {
    constexpr esdb_open_flags known =
        ESDB_OPEN_READONLY | ESDB_OPEN_READWRITE | ESDB_OPEN_CREATE |
        ESDB_OPEN_URI | ESDB_OPEN_FULLMUTEX | ESDB_OPEN_NOMUTEX |
        ESDB_OPEN_NOFOLLOW;
    if ((flags & ~known) != 0u) return false;
    const bool ro = (flags & ESDB_OPEN_READONLY) != 0;
    const bool rw = (flags & ESDB_OPEN_READWRITE) != 0;
    if (ro == rw) return false;
    if (ro && (flags & ESDB_OPEN_CREATE)) return false;
    if ((flags & ESDB_OPEN_FULLMUTEX) && (flags & ESDB_OPEN_NOMUTEX)) return false;
    return true;
}

static esdb_journal_mode parse_journal_mode(const char *text);

static bool valid_journal_request(esdb_journal_mode mode) noexcept {
    switch (mode) {
        case ESDB_JOURNAL_UNCHANGED:
        case ESDB_JOURNAL_DELETE:
        case ESDB_JOURNAL_TRUNCATE:
        case ESDB_JOURNAL_PERSIST:
        case ESDB_JOURNAL_WAL:
            return true;
        case ESDB_JOURNAL_MEMORY:
        case ESDB_JOURNAL_OFF:
        default:
            return false;
    }
}

static esdb_status validate_storage_request(
    const esdb_open_options &options,
    esdb_error *error) noexcept {
    switch (options.storage_mode) {
        case ESDB_STORAGE_DEFAULT:
        case ESDB_STORAGE_PLAIN:
            if (options.storage_provider != ESDB_PROVIDER_AUTO &&
                options.storage_provider != ESDB_PROVIDER_SQLITE) {
                esdb_detail::set_error(
                    error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN,
                    nullptr, SQLITE_MISUSE,
                    "plain storage cannot use a compressed storage provider");
                return ESDB_ERR_INVALID_ARGUMENT;
            }
            if (options.compression_codec != ESDB_CODEC_NONE ||
                options.compression_level != 0) {
                esdb_detail::set_error(
                    error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN,
                    nullptr, SQLITE_MISUSE,
                    "plain storage requires codec=NONE and compression_level=0");
                return ESDB_ERR_INVALID_ARGUMENT;
            }
            return ESDB_OK;

        case ESDB_STORAGE_COMPRESSED:
            if (options.storage_provider != ESDB_PROVIDER_AUTO &&
                options.storage_provider != ESDB_PROVIDER_ZIPVFS) {
                esdb_detail::set_error(
                    error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN,
                    nullptr, SQLITE_MISUSE,
                    "compressed storage requires ZIPVFS or AUTO provider selection");
                return ESDB_ERR_INVALID_ARGUMENT;
            }
            if (options.compression_codec != ESDB_CODEC_ZSTD &&
                options.compression_codec != ESDB_CODEC_DEFLATE) {
                esdb_detail::set_error(
                    error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN,
                    nullptr, SQLITE_MISUSE,
                    "compressed storage requires ZSTD or DEFLATE codec selection");
                return ESDB_ERR_INVALID_ARGUMENT;
            }
            if (!esdb_detail::zipvfs_compiled()) {
                esdb_detail::set_error(
                    error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_OPEN,
                    nullptr, SQLITE_NOTFOUND,
                    "this ESDB build does not contain licensed ZIPVFS support");
                return ESDB_ERR_UNSUPPORTED;
            }
            if (options.compression_codec == ESDB_CODEC_ZSTD &&
                !esdb_detail::zipvfs_zstd_compiled()) {
                esdb_detail::set_error(
                    error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_OPEN,
                    nullptr, SQLITE_NOTFOUND,
                    "this ZIPVFS build does not contain the optional Zstd codec");
                return ESDB_ERR_UNSUPPORTED;
            }
            if (options.journal_mode != ESDB_JOURNAL_UNCHANGED &&
                options.journal_mode != ESDB_JOURNAL_WAL) {
                esdb_detail::set_error(
                    error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_OPEN,
                    nullptr, SQLITE_MISUSE,
                    "ZIPVFS currently supports ESDB journal selection UNCHANGED or WAL");
                return ESDB_ERR_UNSUPPORTED;
            }
            if (options.synchronous != ESDB_SYNCHRONOUS_UNCHANGED) {
                esdb_detail::set_error(
                    error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_OPEN,
                    nullptr, SQLITE_MISUSE,
                    "ZIPVFS lower-pager synchronous tuning is provider-specific; leave ESDB synchronous UNCHANGED");
                return ESDB_ERR_UNSUPPORTED;
            }
            return ESDB_OK;

        default:
            esdb_detail::set_error(
                error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN,
                nullptr, SQLITE_MISUSE, "unknown storage mode");
            return ESDB_ERR_INVALID_ARGUMENT;
    }
}

static bool valid_synchronous_request(esdb_synchronous_mode mode) noexcept {
    switch (mode) {
        case ESDB_SYNCHRONOUS_UNCHANGED:
        case ESDB_SYNCHRONOUS_NORMAL:
        case ESDB_SYNCHRONOUS_FULL:
        case ESDB_SYNCHRONOUS_EXTRA:
            return true;
        case ESDB_SYNCHRONOUS_OFF:
        default:
            return false;
    }
}

static const char *journal_mode_sql(esdb_journal_mode mode) {
    switch (mode) {
        case ESDB_JOURNAL_DELETE: return "PRAGMA journal_mode=DELETE;";
        case ESDB_JOURNAL_TRUNCATE: return "PRAGMA journal_mode=TRUNCATE;";
        case ESDB_JOURNAL_PERSIST: return "PRAGMA journal_mode=PERSIST;";
        case ESDB_JOURNAL_MEMORY: return "PRAGMA journal_mode=MEMORY;";
        case ESDB_JOURNAL_WAL: return "PRAGMA journal_mode=WAL;";
        default: return nullptr;
    }
}

static const char *synchronous_sql(esdb_synchronous_mode mode) {
    switch (mode) {
        case ESDB_SYNCHRONOUS_NORMAL: return "PRAGMA synchronous=NORMAL;";
        case ESDB_SYNCHRONOUS_FULL: return "PRAGMA synchronous=FULL;";
        case ESDB_SYNCHRONOUS_EXTRA: return "PRAGMA synchronous=EXTRA;";
        default: return nullptr;
    }
}

static esdb_status configure_database(esdb_database *database, const esdb_open_options &options, esdb_error *error) {
    sqlite3_extended_result_codes(database->handle, 1);

    if (options.busy_timeout_ms > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_CONFIGURE,
                                 database->handle, SQLITE_RANGE, "busy timeout exceeds SQLite integer range");
    }
    int rc = sqlite3_busy_timeout(database->handle, static_cast<int>(options.busy_timeout_ms));
    if (rc != SQLITE_OK) {
        return esdb_detail::fail(database, error, esdb_detail::map_sqlite_status(rc), ESDB_PHASE_CONFIGURE,
                                 database->handle, rc, nullptr);
    }

    esdb_status status = esdb_detail::exec_sql(
        database, options.foreign_keys ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;",
        ESDB_PHASE_CONFIGURE, error);
    if (status != ESDB_OK) return status;
    std::int64_t actual_foreign_keys = -1;
    status = esdb_detail::query_i64(
        database, "PRAGMA foreign_keys;", &actual_foreign_keys,
        ESDB_PHASE_CONFIGURE, error);
    if (status != ESDB_OK) return status;
    if (actual_foreign_keys != static_cast<std::int64_t>(options.foreign_keys)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_CONFIGURE,
            database->handle, SQLITE_MISMATCH,
            "requested foreign_keys mode could not be applied exactly");
    }

    if (options.journal_mode == ESDB_JOURNAL_OFF ||
        options.journal_mode == ESDB_JOURNAL_MEMORY) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_CONFIGURE,
                                 database->handle, SQLITE_MISUSE,
                                 "non-durable journal mode is not supported by the ESDB durability policy");
    }
    const bool compressed =
        options.storage_mode == ESDB_STORAGE_COMPRESSED;
    const char *journal = compressed && options.journal_mode == ESDB_JOURNAL_WAL
        ? "PRAGMA zipvfs_journal_mode=WAL;"
        : journal_mode_sql(options.journal_mode);
    if (options.journal_mode != ESDB_JOURNAL_UNCHANGED) {
        if (!journal) {
            return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_CONFIGURE,
                                     database->handle, SQLITE_MISUSE, "unknown journal mode");
        }
        status = esdb_detail::exec_sql(database, journal, ESDB_PHASE_CONFIGURE, error);
        if (status != ESDB_OK) return status;

        esdb_detail::Statement journal_query;
        status = journal_query.prepare(
            database,
            compressed ? "PRAGMA zipvfs_journal_mode;" : "PRAGMA journal_mode;",
            ESDB_PHASE_CONFIGURE,
            error);
        if (status != ESDB_OK) return status;
        int step = 0;
        status = journal_query.step(database, &step, ESDB_PHASE_CONFIGURE, error);
        if (status != ESDB_OK) return status;
        const char *actual_text = step == SQLITE_ROW
            ? reinterpret_cast<const char *>(sqlite3_column_text(journal_query.get(), 0))
            : nullptr;
        const esdb_journal_mode actual = parse_journal_mode(actual_text);
        if (actual != options.journal_mode) {
            return esdb_detail::fail(
                database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_CONFIGURE,
                database->handle, SQLITE_MISMATCH,
                "requested journal mode could not be applied exactly");
        }
    }

    if (options.synchronous == ESDB_SYNCHRONOUS_OFF) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_CONFIGURE,
                                 database->handle, SQLITE_MISUSE,
                                 "synchronous=OFF is not supported by the ESDB durability policy");
    }
    const char *synchronous = synchronous_sql(options.synchronous);
    if (options.synchronous != ESDB_SYNCHRONOUS_UNCHANGED) {
        if (!synchronous) {
            return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_CONFIGURE,
                                     database->handle, SQLITE_MISUSE, "unknown synchronous mode");
        }
        status = esdb_detail::exec_sql(database, synchronous, ESDB_PHASE_CONFIGURE, error);
        if (status != ESDB_OK) return status;

        std::int64_t actual_synchronous = -1;
        status = esdb_detail::query_i64(
            database, "PRAGMA synchronous;", &actual_synchronous,
            ESDB_PHASE_CONFIGURE, error);
        if (status != ESDB_OK) return status;
        const std::int64_t expected_synchronous =
            options.synchronous == ESDB_SYNCHRONOUS_NORMAL ? 1 :
            options.synchronous == ESDB_SYNCHRONOUS_FULL ? 2 : 3;
        if (actual_synchronous != expected_synchronous) {
            return esdb_detail::fail(
                database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_CONFIGURE,
                database->handle, SQLITE_MISMATCH,
                "requested synchronous mode could not be applied exactly");
        }
    }

    if (options.cache_kib > 0) {
        char sql[64];
        std::snprintf(sql, sizeof(sql), "PRAGMA cache_size=-%u;", options.cache_kib);
        status = esdb_detail::exec_sql(database, sql, ESDB_PHASE_CONFIGURE, error);
        if (status != ESDB_OK) return status;
        std::int64_t actual_cache = 0;
        status = esdb_detail::query_i64(
            database, "PRAGMA cache_size;", &actual_cache,
            ESDB_PHASE_CONFIGURE, error);
        if (status != ESDB_OK) return status;
        if (actual_cache != -static_cast<std::int64_t>(options.cache_kib)) {
            return esdb_detail::fail(
                database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_CONFIGURE,
                database->handle, SQLITE_MISMATCH,
                "requested cache size could not be applied exactly");
        }
    }

    if (options.wal_autocheckpoint_pages > 0) {
        char sql[64];
        std::snprintf(sql, sizeof(sql), "PRAGMA wal_autocheckpoint=%u;", options.wal_autocheckpoint_pages);
        status = esdb_detail::exec_sql(database, sql, ESDB_PHASE_CONFIGURE, error);
        if (status != ESDB_OK) return status;
        std::int64_t actual_autocheckpoint = 0;
        status = esdb_detail::query_i64(
            database, "PRAGMA wal_autocheckpoint;", &actual_autocheckpoint,
            ESDB_PHASE_CONFIGURE, error);
        if (status != ESDB_OK) return status;
        if (actual_autocheckpoint != static_cast<std::int64_t>(options.wal_autocheckpoint_pages)) {
            return esdb_detail::fail(
                database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_CONFIGURE,
                database->handle, SQLITE_MISMATCH,
                "requested WAL autocheckpoint could not be applied exactly");
        }
    }

    return ESDB_OK;
}

esdb_status esdb_open(const char *path_utf8, const esdb_open_options *options, esdb_database **out_database, esdb_error *error) {
    esdb_detail::clear_error(error);
    if (!path_utf8 || !path_utf8[0] || !out_database) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN, nullptr, SQLITE_MISUSE,
                               "path and out_database are required");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    *out_database = nullptr;
    esdb_open_options resolved{};
    esdb_open_options_init(&resolved);
    if (options) {
        if (options->struct_size != sizeof(esdb_open_options)) {
            esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN, nullptr, SQLITE_MISUSE,
                                   "open options struct_size mismatch");
            return ESDB_ERR_INVALID_ARGUMENT;
        }
        resolved = *options;
    }
    if (!valid_open_flags(resolved.flags)) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN, nullptr, SQLITE_MISUSE,
                               "invalid or unknown open flags");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    if (!valid_journal_request(resolved.journal_mode)) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN, nullptr, SQLITE_MISUSE,
                               "invalid, unknown, or non-durable journal mode");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    if (!valid_synchronous_request(resolved.synchronous)) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN, nullptr, SQLITE_MISUSE,
                               "invalid, unknown, or non-durable synchronous mode");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    const esdb_status storage_status =
        validate_storage_request(resolved, error);
    if (storage_status != ESDB_OK) return storage_status;

    const char *storage_vfs = nullptr;
    const esdb_status vfs_status =
        esdb_detail::storage_vfs_select(resolved, &storage_vfs, error);
    if (vfs_status != ESDB_OK) return vfs_status;

    if (resolved.foreign_keys > 1u) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN, nullptr, SQLITE_MISUSE,
                               "foreign_keys must be 0 or 1");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t reserved : resolved.reserved) {
        if (reserved != 0u) {
            esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN, nullptr, SQLITE_MISUSE,
                                   "reserved open options must be zero");
            return ESDB_ERR_INVALID_ARGUMENT;
        }
    }

    esdb_database *database = new (std::nothrow) esdb_database();
    if (!database) {
        esdb_detail::set_error(error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_OPEN, nullptr, SQLITE_NOMEM,
                               "database allocation failed");
        return ESDB_ERR_OUT_OF_MEMORY;
    }
    database->options = resolved;
    esdb_detail::count_operation(database);

    const int rc = sqlite3_open_v2(
        path_utf8,
        &database->handle,
        sqlite_open_flags(resolved.flags),
        storage_vfs);
    if (rc != SQLITE_OK) {
        const esdb_status status = esdb_detail::map_sqlite_status(rc);
        esdb_detail::fail(database, error, status, ESDB_PHASE_OPEN, database->handle, rc, nullptr);
        if (database->handle) sqlite3_close_v2(database->handle);
        delete database;
        return status;
    }

    const esdb_status verified =
        esdb_detail::storage_verify_open(database->handle, resolved, error);
    if (verified != ESDB_OK) {
        sqlite3_close_v2(database->handle);
        delete database;
        return verified;
    }

    const esdb_status configured = configure_database(database, resolved, error);
    if (configured != ESDB_OK) {
        sqlite3_close_v2(database->handle);
        delete database;
        return configured;
    }
    *out_database = database;
    return ESDB_OK;
}

void esdb_close(esdb_database *database) {
    if (!database) return;
    if (database->handle) {
        sqlite3_close_v2(database->handle);
        database->handle = nullptr;
    }
    delete database;
}

void *esdb_native_handle(esdb_database *database) {
    return database ? static_cast<void *>(database->handle) : nullptr;
}

esdb_status esdb_exec(esdb_database *database, const char *sql_utf8, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    const esdb_status status =
        esdb_detail::exec_sql(database, sql_utf8, ESDB_PHASE_EXEC, error);
    if (database && database->handle) {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        database->transaction_active =
            sqlite3_get_autocommit(database->handle) == 0;
        if (!database->transaction_active) {
            database->savepoint_depth = 0u;
            if (database->active_transaction) {
                database->active_transaction->active = false;
                database->active_transaction = nullptr;
            }
        }
    }
    return status;
}


namespace {

bool esdb_query_tail_is_empty(const char *tail) noexcept {
    if (!tail) return true;
    while (*tail) {
        const unsigned char c = static_cast<unsigned char>(*tail);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v') {
            return false;
        }
        ++tail;
    }
    return true;
}

struct QueryStatementGuard {
    sqlite3_stmt *statement = nullptr;

    ~QueryStatementGuard() noexcept {
        finalize();
    }

    QueryStatementGuard() = default;
    QueryStatementGuard(const QueryStatementGuard &) = delete;
    QueryStatementGuard &operator=(const QueryStatementGuard &) = delete;

    void finalize() noexcept {
        if (statement) {
            sqlite3_finalize(statement);
            statement = nullptr;
        }
    }
};

struct QueryOwnedValueGuard {
    std::vector<esdb_value *> values;

    QueryOwnedValueGuard() = default;
    QueryOwnedValueGuard(const QueryOwnedValueGuard &) = delete;
    QueryOwnedValueGuard &operator=(const QueryOwnedValueGuard &) = delete;

    ~QueryOwnedValueGuard() noexcept {
        clear();
    }

    void clear() noexcept {
        for (esdb_value *value : values) {
            esdb_value_destroy(value);
        }
        values.clear();
    }
};

esdb_status esdb_query_bind_value(
    esdb_database *database,
    sqlite3_stmt *statement,
    int index,
    const esdb_value *value,
    esdb_error *error) noexcept {
    if (!value) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_EXEC,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "query parameter value is null");
    }

    int rc = SQLITE_MISUSE;
    switch (esdb_value_type_of(value)) {
        case ESDB_VALUE_NULL:
            rc = sqlite3_bind_null(statement, index);
            break;
        case ESDB_VALUE_BOOL: {
            int decoded = 0;
            if (esdb_value_get_bool(value, &decoded) != ESDB_OK) {
                return esdb_detail::fail(
                    database, error, ESDB_ERR_TYPE_MISMATCH, ESDB_PHASE_EXEC,
                    database->handle, SQLITE_MISMATCH, "invalid BOOL query parameter");
            }
            rc = sqlite3_bind_int(statement, index, decoded ? 1 : 0);
            break;
        }
        case ESDB_VALUE_INT32: {
            std::int32_t decoded = 0;
            if (esdb_value_get_int32(value, &decoded) != ESDB_OK) {
                return esdb_detail::fail(
                    database, error, ESDB_ERR_TYPE_MISMATCH, ESDB_PHASE_EXEC,
                    database->handle, SQLITE_MISMATCH, "invalid INT32 query parameter");
            }
            rc = sqlite3_bind_int(statement, index, decoded);
            break;
        }
        case ESDB_VALUE_INT64: {
            std::int64_t decoded = 0;
            if (esdb_value_get_int64(value, &decoded) != ESDB_OK) {
                return esdb_detail::fail(
                    database, error, ESDB_ERR_TYPE_MISMATCH, ESDB_PHASE_EXEC,
                    database->handle, SQLITE_MISMATCH, "invalid INT64 query parameter");
            }
            rc = sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(decoded));
            break;
        }
        case ESDB_VALUE_DOUBLE: {
            double decoded = 0.0;
            if (esdb_value_get_double(value, &decoded) != ESDB_OK) {
                return esdb_detail::fail(
                    database, error, ESDB_ERR_TYPE_MISMATCH, ESDB_PHASE_EXEC,
                    database->handle, SQLITE_MISMATCH, "invalid DOUBLE query parameter");
            }
            if (std::isnan(decoded)) {
                return esdb_detail::fail(
                    database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_EXEC,
                    database->handle, SQLITE_MISMATCH,
                    "NaN DOUBLE query parameters are not representable by SQLite");
            }
            rc = sqlite3_bind_double(statement, index, decoded);
            break;
        }
        case ESDB_VALUE_UTF8:
        case ESDB_VALUE_ARRAY:
        case ESDB_VALUE_OBJECT:
        case ESDB_VALUE_BYTES: {
            const void *data = nullptr;
            std::uint64_t size = 0u;
            if (esdb_value_get_data(value, &data, &size) != ESDB_OK) {
                return esdb_detail::fail(
                    database, error, ESDB_ERR_TYPE_MISMATCH, ESDB_PHASE_EXEC,
                    database->handle, SQLITE_MISMATCH, "invalid query parameter payload");
            }
            if (esdb_value_type_of(value) == ESDB_VALUE_BYTES) {
                rc = size == 0u
                    ? sqlite3_bind_zeroblob64(statement, index, 0u)
                    : sqlite3_bind_blob64(
                        statement, index, data, static_cast<sqlite3_uint64>(size), SQLITE_TRANSIENT);
            } else {
                static const char empty[] = "";
                const char *text = size == 0u ? empty : static_cast<const char *>(data);
                rc = sqlite3_bind_text64(
                    statement, index, text, static_cast<sqlite3_uint64>(size),
                    SQLITE_TRANSIENT, SQLITE_UTF8);
            }
            break;
        }
        default:
            return esdb_detail::fail(
                database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_EXEC,
                database->handle, SQLITE_MISMATCH, "unsupported query parameter type");
    }

    if (rc != SQLITE_OK) {
        return esdb_detail::fail(
            database, error, esdb_detail::map_sqlite_status(rc), ESDB_PHASE_EXEC,
            database->handle, rc, nullptr);
    }
    return ESDB_OK;
}

esdb_status esdb_query_column_value(
    esdb_database *database,
    sqlite3_stmt *statement,
    int column,
    esdb_value **out_value,
    esdb_error *error) noexcept {
    if (!out_value) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_EXEC,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "query row output value is required");
    }
    *out_value = nullptr;

    esdb_error value_error{};
    esdb_status status = ESDB_ERR_INTERNAL;
    switch (sqlite3_column_type(statement, column)) {
        case SQLITE_NULL:
            status = esdb_value_create_null(out_value, &value_error);
            break;
        case SQLITE_INTEGER:
            status = esdb_value_create_int64(
                static_cast<std::int64_t>(sqlite3_column_int64(statement, column)),
                out_value, &value_error);
            break;
        case SQLITE_FLOAT:
            status = esdb_value_create_double(
                sqlite3_column_double(statement, column), out_value, &value_error);
            break;
        case SQLITE_TEXT: {
            const unsigned char *text = sqlite3_column_text(statement, column);
            const int bytes = sqlite3_column_bytes(statement, column);
            if (!text && bytes > 0) {
                return esdb_detail::fail(
                    database, error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_EXEC,
                    database->handle, SQLITE_NOMEM, "SQLite could not materialize TEXT column");
            }
            status = esdb_value_create_text(
                ESDB_VALUE_UTF8,
                bytes == 0 ? "" : reinterpret_cast<const char *>(text),
                static_cast<std::uint64_t>(bytes),
                out_value,
                &value_error);
            break;
        }
        case SQLITE_BLOB: {
            const void *blob = sqlite3_column_blob(statement, column);
            const int bytes = sqlite3_column_bytes(statement, column);
            if (!blob && bytes > 0) {
                return esdb_detail::fail(
                    database, error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_EXEC,
                    database->handle, SQLITE_NOMEM, "SQLite could not materialize BLOB column");
            }
            status = esdb_value_create_bytes(
                blob, static_cast<std::uint64_t>(bytes), out_value, &value_error);
            break;
        }
        default:
            status = ESDB_ERR_INTERNAL;
            break;
    }

    if (status != ESDB_OK) {
        const int sqlite_code =
            status == ESDB_ERR_OUT_OF_MEMORY ? SQLITE_NOMEM :
            status == ESDB_ERR_INVALID_ARGUMENT ? SQLITE_MISMATCH : SQLITE_ERROR;
        return esdb_detail::fail(
            database, error, status, ESDB_PHASE_EXEC, database->handle, sqlite_code,
            value_error.message[0] ? value_error.message : "failed to decode query result column");
    }
    return ESDB_OK;
}

void esdb_query_sync_transaction_state(esdb_database *database) noexcept {
    if (!database || !database->handle) return;
    std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
    database->transaction_active = sqlite3_get_autocommit(database->handle) == 0;
    if (!database->transaction_active) {
        database->savepoint_depth = 0u;
        if (database->active_transaction) {
            database->active_transaction->active = false;
            database->active_transaction = nullptr;
        }
    }
}

}  // namespace

esdb_status esdb_query(
    esdb_database *database,
    const char *sql_utf8,
    const esdb_value *const *parameters,
    uint32_t parameter_count,
    esdb_query_row_callback callback,
    void *user_data,
    uint64_t *out_row_count,
    uint64_t *out_change_count,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (out_row_count) *out_row_count = 0u;
    if (out_change_count) *out_change_count = 0u;

    if (!database || !database->handle || !sql_utf8 || !sql_utf8[0] ||
        !esdb_detail::valid_utf8(
            sql_utf8, static_cast<std::uint64_t>(std::strlen(sql_utf8))) ||
        (parameter_count != 0u && !parameters) ||
        parameter_count > static_cast<uint32_t>(INT_MAX)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_EXEC,
            database ? database->handle : nullptr, SQLITE_MISUSE,
            "invalid query arguments");
    }

    QueryStatementGuard statement_guard;
    const char *tail = nullptr;
    int rc = sqlite3_prepare_v3(
        database->handle,
        sql_utf8,
        -1,
        SQLITE_PREPARE_PERSISTENT,
        &statement_guard.statement,
        &tail);
    if (rc != SQLITE_OK) {
        return esdb_detail::fail(
            database, error, esdb_detail::map_sqlite_status(rc), ESDB_PHASE_EXEC,
            database->handle, rc, nullptr);
    }
    sqlite3_stmt *statement = statement_guard.statement;
    if (!statement || !esdb_query_tail_is_empty(tail)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_EXEC,
            database->handle, SQLITE_MISUSE,
            "esdb_query accepts exactly one SQL statement");
    }

    const int expected_parameters = sqlite3_bind_parameter_count(statement);
    if (expected_parameters != static_cast<int>(parameter_count)) {
        return esdb_detail::fail(
            database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_EXEC,
            database->handle, SQLITE_RANGE,
            "query parameter count does not match SQL placeholders");
    }

    for (uint32_t i = 0u; i < parameter_count; ++i) {
        const esdb_status bound = esdb_query_bind_value(
            database, statement, static_cast<int>(i + 1u), parameters[i], error);
        if (bound != ESDB_OK) {
            return bound;
        }
    }

    const bool readonly = sqlite3_stmt_readonly(statement) != 0;
    std::uint64_t rows = 0u;
    bool deliver_rows = callback != nullptr;
    esdb_status status = ESDB_OK;

    try {
        const int column_count_i = sqlite3_column_count(statement);
        const uint32_t column_count =
            column_count_i > 0 ? static_cast<uint32_t>(column_count_i) : 0u;
        std::vector<const char *> column_names(column_count);
        QueryOwnedValueGuard owned_values;
        owned_values.values.resize(column_count, nullptr);
        std::vector<const esdb_value *> borrowed_values(column_count, nullptr);

        for (uint32_t column = 0u; column < column_count; ++column) {
            column_names[column] = sqlite3_column_name(statement, static_cast<int>(column));
        }

        for (;;) {
            rc = sqlite3_step(statement);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                status = esdb_detail::fail(
                    database, error, esdb_detail::map_sqlite_status(rc), ESDB_PHASE_EXEC,
                    database->handle, rc, nullptr);
                break;
            }

            ++rows;
            if (!deliver_rows) continue;

            bool row_ok = true;
            for (uint32_t column = 0u; column < column_count; ++column) {
                esdb_value_destroy(owned_values.values[column]);
                owned_values.values[column] = nullptr;
                borrowed_values[column] = nullptr;
                const esdb_status decoded = esdb_query_column_value(
                    database, statement, static_cast<int>(column),
                    &owned_values.values[column], error);
                if (decoded != ESDB_OK) {
                    status = decoded;
                    row_ok = false;
                    break;
                }
                borrowed_values[column] = owned_values.values[column];
            }
            if (!row_ok) break;

            if (callback(
                    column_names.empty() ? nullptr : column_names.data(),
                    borrowed_values.empty() ? nullptr : borrowed_values.data(),
                    column_count,
                    user_data) != 0) {
                deliver_rows = false;
            }
        }

    } catch (const std::bad_alloc &) {
        status = esdb_detail::fail(
            database, error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_EXEC,
            database->handle, SQLITE_NOMEM, "query row allocation failed");
    } catch (...) {
        status = esdb_detail::fail(
            database, error, ESDB_ERR_INTERNAL, ESDB_PHASE_EXEC,
            database->handle, SQLITE_ERROR, "unexpected exception while executing query");
    }

    if (status == ESDB_OK) {
        if (out_row_count) *out_row_count = rows;
        if (out_change_count && !readonly) {
            const sqlite3_int64 changes = sqlite3_changes64(database->handle);
            *out_change_count = changes > 0 ? static_cast<std::uint64_t>(changes) : 0u;
        }
    }

    statement_guard.finalize();
    esdb_query_sync_transaction_state(database);
    return status;
}

esdb_status esdb_data_version_get(esdb_database *database, uint32_t *out_data_version, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!out_data_version) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_HEALTH,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "out_data_version is required");
    }
    std::uint64_t value = 0;
    const esdb_status status = esdb_detail::query_u64(database, "PRAGMA data_version;", &value, ESDB_PHASE_HEALTH, error);
    if (status != ESDB_OK) return status;
    if (value > std::numeric_limits<uint32_t>::max()) {
        return esdb_detail::fail(database, error, ESDB_ERR_SQLITE, ESDB_PHASE_HEALTH,
                                 database->handle, SQLITE_RANGE, "data_version exceeds uint32 range");
    }
    *out_data_version = static_cast<uint32_t>(value);
    return ESDB_OK;
}

/* ---- transactions ---- */

esdb_status esdb_begin(esdb_database *database, esdb_transaction_mode mode, esdb_transaction **out_transaction, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle || !out_transaction) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_TRANSACTION,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "invalid transaction arguments");
    }
    *out_transaction = nullptr;
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        if (database->transaction_active) {
            return esdb_detail::fail(database, error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_TRANSACTION,
                                     database->handle, SQLITE_MISUSE,
                                     "an ESDB transaction is already active on this connection");
        }
    }

    const char *sql = nullptr;
    switch (mode) {
        case ESDB_TRANSACTION_DEFERRED: sql = "BEGIN DEFERRED;"; break;
        case ESDB_TRANSACTION_IMMEDIATE: sql = "BEGIN IMMEDIATE;"; break;
        case ESDB_TRANSACTION_EXCLUSIVE: sql = "BEGIN EXCLUSIVE;"; break;
        default: break;
    }
    if (!sql) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_TRANSACTION,
                                 database->handle, SQLITE_MISUSE, "unknown transaction mode");
    }

    const esdb_status status = esdb_detail::exec_sql(database, sql, ESDB_PHASE_TRANSACTION, error);
    if (status != ESDB_OK) return status;

    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        database->transaction_active = true;
    }

    esdb_transaction *transaction = new (std::nothrow) esdb_transaction();
    if (!transaction) {
        esdb_detail::exec_sql(database, "ROLLBACK;", ESDB_PHASE_TRANSACTION, nullptr);
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        database->transaction_active = false;
        return esdb_detail::fail(database, error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_TRANSACTION,
                                 database->handle, SQLITE_NOMEM, "transaction allocation failed");
    }
    transaction->database = database;
    transaction->active = true;
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        database->active_transaction = transaction;
    }
    *out_transaction = transaction;
    return ESDB_OK;
}

static esdb_status finish_transaction(esdb_transaction *transaction, bool commit, esdb_error *error) {
    esdb_detail::clear_error(error);
    if (!transaction || !transaction->database || !transaction->active) {
        return esdb_detail::fail(transaction ? transaction->database : nullptr, error,
                                 ESDB_ERR_INVALID_STATE, ESDB_PHASE_TRANSACTION,
                                 transaction && transaction->database ? transaction->database->handle : nullptr,
                                 SQLITE_MISUSE, "transaction is not active");
    }
    esdb_database *database = transaction->database;
    const esdb_status status = esdb_detail::exec_sql(
        database, commit ? "COMMIT;" : "ROLLBACK;", ESDB_PHASE_TRANSACTION, error);
    if (status == ESDB_OK) {
        transaction->active = false;
        {
            std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
            database->transaction_active = false;
            database->savepoint_depth = 0u;
            if (database->active_transaction == transaction) {
                database->active_transaction = nullptr;
            }
        }
        if (commit) esdb_detail::count_commit(database);
        else esdb_detail::count_rollback(database);
    } else {
        // A failed COMMIT (for example SQLITE_BUSY) can leave the transaction
        // open; keep the flags truthful by asking SQLite.
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        database->transaction_active = sqlite3_get_autocommit(database->handle) == 0;
        if (!database->transaction_active) {
            database->savepoint_depth = 0u;
            transaction->active = false;
            if (database->active_transaction == transaction) {
                database->active_transaction = nullptr;
            }
        }
    }
    return status;
}

esdb_status esdb_commit(esdb_transaction *transaction, esdb_error *error) {
    esdb_detail::count_operation(transaction ? transaction->database : nullptr);
    return finish_transaction(transaction, true, error);
}

esdb_status esdb_rollback(esdb_transaction *transaction, esdb_error *error) {
    esdb_detail::count_operation(transaction ? transaction->database : nullptr);
    return finish_transaction(transaction, false, error);
}

void esdb_transaction_destroy(esdb_transaction *transaction) {
    if (!transaction) return;
    if (transaction->active && transaction->database) {
        finish_transaction(transaction, false, nullptr);
    }
    delete transaction;
}

int esdb_transaction_active(const esdb_transaction *transaction) {
    return transaction && transaction->active ? 1 : 0;
}

/* ---- savepoints ---- */

esdb_status esdb_savepoint_begin(esdb_database *database, const char *name, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle || !esdb_detail::valid_savepoint_name(name)) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_SAVEPOINT,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "invalid savepoint name (expected [A-Za-z0-9_], 1..64 bytes)");
    }
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        if (!database->transaction_active) {
            return esdb_detail::fail(database, error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_SAVEPOINT,
                                     database->handle, SQLITE_MISUSE,
                                     "savepoints require an active transaction");
        }
        if (database->savepoint_depth >= database->savepoints.size()) {
            return esdb_detail::fail(database, error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_SAVEPOINT,
                                     database->handle, SQLITE_MISUSE, "savepoint depth limit reached");
        }
    }
    char sql[128];
    std::snprintf(sql, sizeof(sql), "SAVEPOINT %s;", name);
    const esdb_status status = esdb_detail::exec_sql(database, sql, ESDB_PHASE_SAVEPOINT, error);
    if (status != ESDB_OK) return status;
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        auto &slot = database->savepoints[database->savepoint_depth];
        std::memset(slot.data(), 0, slot.size());
        std::memcpy(slot.data(), name, std::strlen(name));
        ++database->savepoint_depth;
    }
    return ESDB_OK;
}

esdb_status esdb_savepoint_release(esdb_database *database, const char *name, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle || !esdb_detail::valid_savepoint_name(name)) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_SAVEPOINT,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "invalid savepoint name (expected [A-Za-z0-9_], 1..64 bytes)");
    }
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        if (database->savepoint_depth == 0u ||
            std::strcmp(database->savepoints[database->savepoint_depth - 1u].data(), name) != 0) {
            return esdb_detail::fail(database, error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_SAVEPOINT,
                                     database->handle, SQLITE_MISUSE,
                                     "savepoint release must match the most recent active savepoint");
        }
    }
    char sql[128];
    std::snprintf(sql, sizeof(sql), "RELEASE %s;", name);
    const esdb_status status = esdb_detail::exec_sql(database, sql, ESDB_PHASE_SAVEPOINT, error);
    if (status != ESDB_OK) return status;
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        if (database->savepoint_depth > 0u) --database->savepoint_depth;
    }
    return ESDB_OK;
}

esdb_status esdb_savepoint_rollback(esdb_database *database, const char *name, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle || !esdb_detail::valid_savepoint_name(name)) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_SAVEPOINT,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "invalid savepoint name (expected [A-Za-z0-9_], 1..64 bytes)");
    }
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        if (database->savepoint_depth == 0u ||
            std::strcmp(database->savepoints[database->savepoint_depth - 1u].data(), name) != 0) {
            return esdb_detail::fail(database, error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_SAVEPOINT,
                                     database->handle, SQLITE_MISUSE,
                                     "savepoint rollback must match the most recent active savepoint");
        }
    }
    char sql[160];
    std::snprintf(sql, sizeof(sql), "ROLLBACK TO %s; RELEASE %s;", name, name);
    const esdb_status status = esdb_detail::exec_sql(database, sql, ESDB_PHASE_SAVEPOINT, error);
    if (status != ESDB_OK) return status;
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        if (database->savepoint_depth > 0u) --database->savepoint_depth;
    }
    return ESDB_OK;
}

uint32_t esdb_savepoint_depth(const esdb_database *database) {
    if (!database) return 0u;
    auto *mutable_database = const_cast<esdb_database *>(database);
    std::lock_guard<esdb_detail::NoThrowMutex> lock(mutable_database->state_mutex);
    return mutable_database->savepoint_depth;
}

/* ---- schema versioning and migrations ---- */

esdb_status esdb_user_version_get(esdb_database *database, uint32_t *out_version, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!out_version) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_MIGRATION,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "out_version is required");
    }
    std::uint64_t value = 0;
    const esdb_status status = esdb_detail::query_u64(database, "PRAGMA user_version;", &value, ESDB_PHASE_MIGRATION, error);
    if (status != ESDB_OK) return status;
    if (value > std::numeric_limits<uint32_t>::max()) {
        return esdb_detail::fail(database, error, ESDB_ERR_MIGRATION, ESDB_PHASE_MIGRATION,
                                 database->handle, SQLITE_RANGE, "user_version exceeds uint32 range");
    }
    *out_version = static_cast<uint32_t>(value);
    return ESDB_OK;
}

esdb_status esdb_user_version_set(esdb_database *database, uint32_t version, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_MIGRATION,
                                 database ? database->handle : nullptr, SQLITE_MISUSE, "database is required");
    }
    char sql[64];
    std::snprintf(sql, sizeof(sql), "PRAGMA user_version=%u;", version);
    return esdb_detail::exec_sql(database, sql, ESDB_PHASE_MIGRATION, error);
}

esdb_status esdb_migrate(
    esdb_database *database,
    uint32_t target_version,
    const esdb_migration *migrations,
    uint32_t migration_count,
    esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle || (migration_count > 0 && !migrations)) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_MIGRATION,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "invalid migration arguments");
    }
    uint32_t current = 0;
    esdb_status status = esdb_user_version_get(database, &current, error);
    if (status != ESDB_OK) return status;
    if (current == target_version) return ESDB_OK;
    if (current > target_version) {
        return esdb_detail::fail(database, error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_MIGRATION,
                                 database->handle, SQLITE_MISUSE,
                                 "downgrade migrations are not supported by esdb_migrate");
    }

    esdb_transaction *transaction = nullptr;
    status = esdb_begin(database, ESDB_TRANSACTION_IMMEDIATE, &transaction, error);
    if (status != ESDB_OK) return status;

    while (current < target_version) {
        const esdb_migration *match = nullptr;
        for (uint32_t i = 0; i < migration_count; ++i) {
            if (migrations[i].from_version == current && migrations[i].to_version == current + 1u &&
                migrations[i].apply) {
                match = &migrations[i];
                break;
            }
        }
        if (!match) {
            esdb_rollback(transaction, nullptr);
            esdb_transaction_destroy(transaction);
            return esdb_detail::fail(database, error, ESDB_ERR_MIGRATION, ESDB_PHASE_MIGRATION,
                                     database->handle, SQLITE_NOTFOUND,
                                     "missing contiguous migration step");
        }
        try {
            status = match->apply(database, match->from_version, match->to_version, match->user_data, error);
        } catch (...) {
            esdb_rollback(transaction, nullptr);
            esdb_transaction_destroy(transaction);
            return esdb_detail::fail(database, error, ESDB_ERR_INTERNAL, ESDB_PHASE_MIGRATION,
                                     database->handle, SQLITE_ABORT,
                                     "migration callback threw an exception");
        }
        if (status != ESDB_OK) {
            esdb_error callback_error{};
            if (error) callback_error = *error;

            esdb_rollback(transaction, nullptr);
            esdb_transaction_destroy(transaction);

            const int sqlite_code =
                callback_error.sqlite_code != SQLITE_OK
                    ? callback_error.sqlite_code
                    : SQLITE_ERROR;
            const char *message =
                callback_error.message[0] != '\0'
                    ? callback_error.message
                    : "migration callback failed";
            return esdb_detail::fail(
                database, error, status, ESDB_PHASE_MIGRATION,
                database->handle, sqlite_code, message);
        }
        status = esdb_user_version_set(database, match->to_version, error);
        if (status != ESDB_OK) {
            esdb_rollback(transaction, nullptr);
            esdb_transaction_destroy(transaction);
            return status;
        }
        current = match->to_version;
    }

    status = esdb_commit(transaction, error);
    esdb_transaction_destroy(transaction);
    return status;
}

/* ---- integrity / backup / health ---- */

esdb_status esdb_integrity_check(esdb_database *database, int quick, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_INTEGRITY,
                                 nullptr, SQLITE_MISUSE, "database is required");
    }
    esdb_detail::Statement statement;
    esdb_status status = statement.prepare(
        database, quick ? "PRAGMA quick_check;" : "PRAGMA integrity_check;", ESDB_PHASE_INTEGRITY, error);
    if (status != ESDB_OK) return status;

    bool ok = true;
    char problem[ESDB_ERROR_MESSAGE_CAPACITY]{};
    int step = 0;
    while (true) {
        status = statement.step(database, &step, ESDB_PHASE_INTEGRITY, error);
        if (status != ESDB_OK) return status;
        if (step != SQLITE_ROW) break;
        const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
        if (!text || std::strcmp(text, "ok") != 0) {
            ok = false;
            if (problem[0] == '\0' && text) {
#if defined(_MSC_VER)
                strncpy_s(problem, sizeof(problem), text, _TRUNCATE);
#else
                std::strncpy(problem, text, sizeof(problem) - 1u);
                problem[sizeof(problem) - 1u] = '\0';
#endif
            }
        }
    }
    if (!ok) {
        return esdb_detail::fail(database, error, ESDB_ERR_CORRUPT, ESDB_PHASE_INTEGRITY,
                                 database->handle, SQLITE_CORRUPT,
                                  problem[0] == '\0' ? "SQLite integrity check failed" : problem);
    }
    return ESDB_OK;
}

esdb_status esdb_backup_to(esdb_database *database, const char *target_path_utf8, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle || !target_path_utf8 || !target_path_utf8[0]) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_BACKUP,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "database and target path are required");
    }
    sqlite3 *target = nullptr;
    int rc = sqlite3_open_v2(
        target_path_utf8, &target,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        const esdb_status status = esdb_detail::map_sqlite_status(rc);
        esdb_detail::fail(database, error, status, ESDB_PHASE_BACKUP, target, rc, nullptr);
        if (target) sqlite3_close_v2(target);
        return status;
    }
    sqlite3_backup *backup = sqlite3_backup_init(target, "main", database->handle, "main");
    if (!backup) {
        rc = sqlite3_errcode(target);
        const esdb_status status = esdb_detail::map_sqlite_status(rc);
        esdb_detail::fail(database, error, status, ESDB_PHASE_BACKUP, target, rc, nullptr);
        sqlite3_close_v2(target);
        return status;
    }
    unsigned retries = 0;
    do {
        rc = sqlite3_backup_step(backup, -1);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            if (++retries > 200u) break;
            sqlite3_sleep(10);
        }
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    const int finish_rc = sqlite3_backup_finish(backup);
    if (finish_rc != SQLITE_OK) rc = finish_rc;
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
        const esdb_status status = esdb_detail::map_sqlite_status(rc);
        esdb_detail::fail(database, error, status, ESDB_PHASE_BACKUP, target, rc, nullptr);
        sqlite3_close_v2(target);
        return status;
    }
    rc = sqlite3_close_v2(target);
    if (rc != SQLITE_OK) {
        const esdb_status status = esdb_detail::map_sqlite_status(rc);
        esdb_detail::fail(database, error, status, ESDB_PHASE_BACKUP, target, rc,
                          "failed to close backup database");
        return status;
    }
    return ESDB_OK;
}

static esdb_journal_mode parse_journal_mode(const char *text) {
    if (!text) return ESDB_JOURNAL_UNCHANGED;
    if (std::strcmp(text, "delete") == 0) return ESDB_JOURNAL_DELETE;
    if (std::strcmp(text, "truncate") == 0) return ESDB_JOURNAL_TRUNCATE;
    if (std::strcmp(text, "persist") == 0) return ESDB_JOURNAL_PERSIST;
    if (std::strcmp(text, "memory") == 0) return ESDB_JOURNAL_MEMORY;
    if (std::strcmp(text, "wal") == 0) return ESDB_JOURNAL_WAL;
    if (std::strcmp(text, "off") == 0) return ESDB_JOURNAL_OFF;
    return ESDB_JOURNAL_UNCHANGED;
}

static esdb_synchronous_mode parse_synchronous(std::int64_t raw) {
    switch (raw) {
        case 0: return ESDB_SYNCHRONOUS_OFF;
        case 1: return ESDB_SYNCHRONOUS_NORMAL;
        case 2: return ESDB_SYNCHRONOUS_FULL;
        case 3: return ESDB_SYNCHRONOUS_EXTRA;
        default: return ESDB_SYNCHRONOUS_UNCHANGED;
    }
}

esdb_status esdb_database_health_get(esdb_database *database, esdb_database_health *out_health, esdb_error *error) {
    esdb_detail::count_operation(database);
    esdb_detail::clear_error(error);
    if (!database || !database->handle || !out_health) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_HEALTH,
                                 database ? database->handle : nullptr, SQLITE_MISUSE,
                                 "database and out_health are required");
    }
    if (out_health->struct_size != 0 && out_health->struct_size != sizeof(*out_health)) {
        return esdb_detail::fail(database, error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_HEALTH,
                                 database->handle, SQLITE_MISUSE, "health struct_size mismatch");
    }
    std::memset(out_health, 0, sizeof(*out_health));
    out_health->struct_size = sizeof(*out_health);

    std::uint64_t page_size = 0;
    esdb_status status = esdb_detail::query_u64(database, "PRAGMA page_size;", &page_size, ESDB_PHASE_HEALTH, error);
    if (status != ESDB_OK) return status;
    out_health->page_size = static_cast<uint32_t>(page_size);

    status = esdb_detail::query_u64(database, "PRAGMA page_count;", &out_health->page_count, ESDB_PHASE_HEALTH, error);
    if (status != ESDB_OK) return status;
    status = esdb_detail::query_u64(database, "PRAGMA freelist_count;", &out_health->freelist_count, ESDB_PHASE_HEALTH, error);
    if (status != ESDB_OK) return status;
    out_health->database_bytes = static_cast<std::uint64_t>(out_health->page_size) * out_health->page_count;

    std::int64_t cache = 0;
    status = esdb_detail::query_i64(database, "PRAGMA cache_size;", &cache, ESDB_PHASE_HEALTH, error);
    if (status != ESDB_OK) return status;
    if (cache < 0) {
        out_health->configured_cache_kib = -cache;
    } else if (cache > 0 && page_size > 0) {
        out_health->configured_cache_kib = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(cache) * page_size) / 1024u);
    }

    std::int64_t synchronous = 0;
    status = esdb_detail::query_i64(database, "PRAGMA synchronous;", &synchronous, ESDB_PHASE_HEALTH, error);
    if (status != ESDB_OK) return status;
    out_health->synchronous = parse_synchronous(synchronous);

    out_health->storage_mode = database->options.storage_mode;
    out_health->storage_provider =
        database->options.storage_mode == ESDB_STORAGE_COMPRESSED
            ? ESDB_PROVIDER_ZIPVFS
            : ESDB_PROVIDER_SQLITE;
    out_health->compression_codec =
        database->options.storage_mode == ESDB_STORAGE_COMPRESSED
            ? database->options.compression_codec
            : static_cast<esdb_codec>(ESDB_CODEC_NONE);
    out_health->compression_level =
        database->options.storage_mode == ESDB_STORAGE_COMPRESSED
            ? database->options.compression_level
            : 0;

    std::int64_t busy_timeout = 0;
    status = esdb_detail::query_i64(database, "PRAGMA busy_timeout;", &busy_timeout, ESDB_PHASE_HEALTH, error);
    if (status != ESDB_OK) return status;
    out_health->busy_timeout_ms = busy_timeout > 0 ? static_cast<uint32_t>(busy_timeout) : 0u;

    status = esdb_user_version_get(database, &out_health->user_version, error);
    if (status != ESDB_OK) return status;

    {
        esdb_detail::Statement statement;
        status = statement.prepare(
            database,
            database->options.storage_mode == ESDB_STORAGE_COMPRESSED
                ? "PRAGMA zipvfs_journal_mode;"
                : "PRAGMA journal_mode;",
            ESDB_PHASE_HEALTH,
            error);
        if (status != ESDB_OK) return status;
        int step = 0;
        status = statement.step(database, &step, ESDB_PHASE_HEALTH, error);
        if (status != ESDB_OK) return status;
        const char *mode_text = step == SQLITE_ROW
            ? reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0))
            : nullptr;
        out_health->journal_mode = parse_journal_mode(mode_text);
    }

    out_health->operation_count = database->operation_count.load(std::memory_order_relaxed);
    out_health->error_count = database->error_count.load(std::memory_order_relaxed);
    out_health->busy_count = database->busy_count.load(std::memory_order_relaxed);
    out_health->commit_count = database->commit_count.load(std::memory_order_relaxed);
    out_health->rollback_count = database->rollback_count.load(std::memory_order_relaxed);

    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->state_mutex);
        out_health->in_transaction = database->transaction_active ? 1u : 0u;
        out_health->savepoint_depth = database->savepoint_depth;
    }
    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(database->error_mutex);
        out_health->last_error_status = database->last_error.status;
        out_health->last_error_phase = database->last_error.phase;
        out_health->last_error_sqlite_code = database->last_error.sqlite_code;
        out_health->last_error_sqlite_extended_code = database->last_error.sqlite_extended_code;
    }
    return ESDB_OK;
}
