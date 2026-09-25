#ifndef ESDB_ESDB_HPP
#define ESDB_ESDB_HPP

#include "esdb.h"
#include "esdb_store.h"
#include "esdb_object_store.h"

#include <cstdint>
#include <cstring>
#include <utility>

#if defined(__cplusplus) && __cplusplus >= 201703L
#define ESDB_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define ESDB_NODISCARD __attribute__((warn_unused_result))
#else
#define ESDB_NODISCARD
#endif

namespace esdb {

using Status = esdb_status;
using Phase = esdb_phase;
using ValueType = esdb_value_type;

/*
 * Non-throwing diagnostic storage for the C++ facade.
 *
 * The facade never turns ESDB failures into C++ exceptions: it is designed
 * for Adobe plug-in hosts and product code that chooses its own exception
 * policy above ESDB. Every fallible method returns a Status and optionally
 * fills an Error.
 */
class Error final {
public:
    Error() noexcept {
        esdb_error_clear(&value_);
    }

    ESDB_NODISCARD Status status() const noexcept {
        return value_.status;
    }

    ESDB_NODISCARD Phase phase() const noexcept {
        return value_.phase;
    }

    ESDB_NODISCARD std::int32_t sqlite_code() const noexcept {
        return value_.sqlite_code;
    }

    ESDB_NODISCARD std::int32_t sqlite_extended_code() const noexcept {
        return value_.sqlite_extended_code;
    }

    ESDB_NODISCARD const char *message() const noexcept {
        return value_.message;
    }

    ESDB_NODISCARD const esdb_error &native() const noexcept {
        return value_;
    }

    ESDB_NODISCARD bool ok() const noexcept {
        return value_.status == ESDB_OK;
    }

private:
    friend class Database;
    friend class Transaction;
    friend class Savepoint;
    friend class Value;
    friend class Store;

    esdb_error *output() noexcept {
        esdb_error_clear(&value_);
        return &value_;
    }

    esdb_error value_{};
};

/*
 * Owned ESDB value. Move-only; destroying or resetting releases the value.
 */
class Value final {
public:
    Value() noexcept = default;

    explicit Value(esdb_value *native) noexcept
        : native_(native) {}

    ~Value() noexcept {
        reset();
    }

    Value(const Value &) = delete;
    Value &operator=(const Value &) = delete;

    Value(Value &&other) noexcept
        : native_(other.release()) {}

    Value &operator=(Value &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ESDB_NODISCARD explicit operator bool() const noexcept {
        return native_ != nullptr;
    }

    ESDB_NODISCARD esdb_value *native_handle() const noexcept {
        return native_;
    }

    ESDB_NODISCARD ValueType type() const noexcept {
        return esdb_value_type_of(native_);
    }

    ESDB_NODISCARD bool is_null() const noexcept {
        return type() == ESDB_VALUE_NULL;
    }

    esdb_value *release() noexcept {
        esdb_value *result = native_;
        native_ = nullptr;
        return result;
    }

    void reset(esdb_value *replacement = nullptr) noexcept {
        if (native_) {
            esdb_value_destroy(native_);
        }
        native_ = replacement;
    }

    static Status null_value(Value &out, Error *error = nullptr) noexcept {
        return adopt(esdb_value_create_null(out.scratch(), error ? error->output() : nullptr), out);
    }

    static Status boolean(bool value, Value &out, Error *error = nullptr) noexcept {
        return adopt(esdb_value_create_bool(value ? 1 : 0, out.scratch(), error ? error->output() : nullptr), out);
    }

    static Status int32(std::int32_t value, Value &out, Error *error = nullptr) noexcept {
        return adopt(esdb_value_create_int32(value, out.scratch(), error ? error->output() : nullptr), out);
    }

    static Status int64(std::int64_t value, Value &out, Error *error = nullptr) noexcept {
        return adopt(esdb_value_create_int64(value, out.scratch(), error ? error->output() : nullptr), out);
    }

    static Status number(double value, Value &out, Error *error = nullptr) noexcept {
        return adopt(esdb_value_create_double(value, out.scratch(), error ? error->output() : nullptr), out);
    }

    static Status utf8(const char *text, std::uint64_t size, Value &out, Error *error = nullptr) noexcept {
        return adopt(
            esdb_value_create_text(ESDB_VALUE_UTF8, text, size, out.scratch(), error ? error->output() : nullptr),
            out);
    }

    static Status utf8(const char *text, Value &out, Error *error = nullptr) noexcept {
        return utf8(text, text ? static_cast<std::uint64_t>(std::strlen(text)) : 0u, out, error);
    }

    static Status json_object(const char *text, std::uint64_t size, Value &out, Error *error = nullptr) noexcept {
        return adopt(
            esdb_value_create_text(ESDB_VALUE_OBJECT, text, size, out.scratch(), error ? error->output() : nullptr),
            out);
    }

    static Status json_array(const char *text, std::uint64_t size, Value &out, Error *error = nullptr) noexcept {
        return adopt(
            esdb_value_create_text(ESDB_VALUE_ARRAY, text, size, out.scratch(), error ? error->output() : nullptr),
            out);
    }

    static Status bytes(const void *data, std::uint64_t size, Value &out, Error *error = nullptr) noexcept {
        return adopt(
            esdb_value_create_bytes(data, size, out.scratch(), error ? error->output() : nullptr),
            out);
    }

    ESDB_NODISCARD Status get_bool(bool &out, Error *error = nullptr) const noexcept {
        if (error) error->output();
        int value = 0;
        const Status status = esdb_value_get_bool(native_, &value);
        if (status != ESDB_OK) {
            report(status, error);
            return status;
        }
        out = value != 0;
        return ESDB_OK;
    }

    ESDB_NODISCARD Status get_int32(std::int32_t &out, Error *error = nullptr) const noexcept {
        if (error) error->output();
        const Status status = esdb_value_get_int32(native_, &out);
        if (status != ESDB_OK) {
            report(status, error);
        }
        return status;
    }

    ESDB_NODISCARD Status get_int64(std::int64_t &out, Error *error = nullptr) const noexcept {
        if (error) error->output();
        const Status status = esdb_value_get_int64(native_, &out);
        if (status != ESDB_OK) {
            report(status, error);
        }
        return status;
    }

    ESDB_NODISCARD Status get_double(double &out, Error *error = nullptr) const noexcept {
        if (error) error->output();
        const Status status = esdb_value_get_double(native_, &out);
        if (status != ESDB_OK) {
            report(status, error);
        }
        return status;
    }

    ESDB_NODISCARD Status data(const void **out_data, std::uint64_t &out_size, Error *error = nullptr) const noexcept {
        if (error) error->output();
        const Status status = esdb_value_get_data(native_, out_data, &out_size);
        if (status != ESDB_OK) {
            report(status, error);
        }
        return status;
    }

private:
    esdb_value **scratch() noexcept {
        reset();
        return &native_;
    }

    static Status adopt(Status status, Value &out) noexcept {
        if (status != ESDB_OK) {
            out.reset();
        }
        return status;
    }

    static void report(Status status, Error *error) noexcept {
        if (!error || status == ESDB_OK) return;
        error->output();
        error->value_.status = status;
        error->value_.phase = ESDB_PHASE_VALUE;
    }

    esdb_value *native_ = nullptr;
};

/*
 * Owned handle to ESDB's process-memory state plane. Named Stores live in the
 * shared ESDB core registry, so separate native/ExternalObject callers in one
 * process observe the same state when they resolve the same ESDBCore module.
 */
class Store final {
public:
    Store() noexcept = default;

    explicit Store(esdb_store *native) noexcept
        : native_(native) {}

    ~Store() noexcept {
        reset();
    }

    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    Store(Store &&other) noexcept
        : native_(other.release()) {}

    Store &operator=(Store &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ESDB_NODISCARD explicit operator bool() const noexcept {
        return native_ != nullptr;
    }

    ESDB_NODISCARD esdb_store *native_handle() const noexcept {
        return native_;
    }

    ESDB_NODISCARD const char *name() const noexcept {
        return esdb_store_name(native_);
    }

    esdb_store *release() noexcept {
        esdb_store *result = native_;
        native_ = nullptr;
        return result;
    }

    void reset(esdb_store *replacement = nullptr) noexcept {
        if (native_) {
            esdb_store_close(native_);
        }
        native_ = replacement;
    }

    static Status open(
        const char *name_utf8,
        Store &out,
        Error *error = nullptr) noexcept {
        esdb_store *native = nullptr;
        const Status status =
            esdb_store_open(name_utf8, &native, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            out.reset(native);
        }
        return status;
    }

    static Status destroy(
        const char *name_utf8,
        Error *error = nullptr) noexcept {
        return esdb_store_destroy(name_utf8, error ? error->output() : nullptr);
    }

    Status put(
        const char *key_utf8,
        const Value &value,
        std::uint64_t *out_revision = nullptr,
        Error *error = nullptr) noexcept {
        return esdb_store_put(
            native_, key_utf8, value.native_handle(), out_revision,
            error ? error->output() : nullptr);
    }

    Status patch(
        const esdb_store_patch_entry *entries,
        std::uint32_t count,
        std::uint64_t *out_revision = nullptr,
        Error *error = nullptr) noexcept {
        return esdb_store_patch(
            native_, entries, count, out_revision,
            error ? error->output() : nullptr);
    }

    Status get(
        const char *key_utf8,
        Value &out,
        Error *error = nullptr) noexcept {
        esdb_value *native = nullptr;
        const Status status = esdb_store_get(
            native_, key_utf8, &native, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            out.reset(native);
        }
        return status;
    }

    Status erase(
        const char *key_utf8,
        bool *out_deleted = nullptr,
        std::uint64_t *out_revision = nullptr,
        Error *error = nullptr) noexcept {
        int deleted = 0;
        const Status status = esdb_store_delete(
            native_, key_utf8, &deleted, out_revision,
            error ? error->output() : nullptr);
        if (status == ESDB_OK && out_deleted) {
            *out_deleted = deleted != 0;
        }
        return status;
    }

    Status exists(
        const char *key_utf8,
        bool &out_exists,
        Error *error = nullptr) noexcept {
        int exists = 0;
        const Status status = esdb_store_exists(
            native_, key_utf8, &exists, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            out_exists = exists != 0;
        }
        return status;
    }

    Status count(
        std::uint64_t &out_count,
        Error *error = nullptr) noexcept {
        return esdb_store_count(
            native_, &out_count, error ? error->output() : nullptr);
    }

    Status clear(
        bool *out_cleared = nullptr,
        std::uint64_t *out_revision = nullptr,
        Error *error = nullptr) noexcept {
        int cleared = 0;
        const Status status = esdb_store_clear(
            native_, &cleared, out_revision,
            error ? error->output() : nullptr);
        if (status == ESDB_OK && out_cleared) {
            *out_cleared = cleared != 0;
        }
        return status;
    }

    Status revision(
        std::uint64_t &out_revision,
        Error *error = nullptr) const noexcept {
        return esdb_store_revision(
            native_, &out_revision, error ? error->output() : nullptr);
    }

    Status retained_floor(
        std::uint64_t &out_revision,
        Error *error = nullptr) const noexcept {
        return esdb_store_retained_floor(
            native_, &out_revision, error ? error->output() : nullptr);
    }

private:
    esdb_store *native_ = nullptr;
};

class Transaction final {
public:
    Transaction() noexcept = default;

    explicit Transaction(esdb_transaction *native) noexcept
        : native_(native) {}

    ~Transaction() noexcept {
        reset();
    }

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    Transaction(Transaction &&other) noexcept
        : native_(other.release()) {}

    Transaction &operator=(Transaction &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ESDB_NODISCARD explicit operator bool() const noexcept {
        return active();
    }

    ESDB_NODISCARD bool active() const noexcept {
        return native_ != nullptr && esdb_transaction_active(native_) != 0;
    }

    ESDB_NODISCARD esdb_transaction *native_handle() const noexcept {
        return native_;
    }

    esdb_transaction *release() noexcept {
        esdb_transaction *result = native_;
        native_ = nullptr;
        return result;
    }

    void reset(esdb_transaction *replacement = nullptr) noexcept {
        if (native_) {
            esdb_transaction_destroy(native_);
        }
        native_ = replacement;
    }

    Status commit(Error *error = nullptr) noexcept {
        const Status status = esdb_commit(native_, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            reset();
        }
        return status;
    }

    Status rollback(Error *error = nullptr) noexcept {
        const Status status = esdb_rollback(native_, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            reset();
        }
        return status;
    }

private:
    esdb_transaction *native_ = nullptr;
};

class Database;

/* RAII savepoint: destructor rolls back when the savepoint is still active. */
class Savepoint final {
public:
    Savepoint() noexcept = default;

    ~Savepoint() noexcept {
        reset();
    }

    Savepoint(const Savepoint &) = delete;
    Savepoint &operator=(const Savepoint &) = delete;

    Savepoint(Savepoint &&other) noexcept
        : database_(other.database_), active_(other.active_) {
        std::memcpy(name_, other.name_, sizeof(name_));
        other.database_ = nullptr;
        other.active_ = false;
    }

    Savepoint &operator=(Savepoint &&other) noexcept {
        if (this != &other) {
            reset();
            database_ = other.database_;
            active_ = other.active_;
            std::memcpy(name_, other.name_, sizeof(name_));
            other.database_ = nullptr;
            other.active_ = false;
        }
        return *this;
    }

    static Status begin(Database &database, const char *name, Savepoint &out, Error *error = nullptr) noexcept;

    ESDB_NODISCARD bool active() const noexcept {
        return active_;
    }

    Status release(Error *error = nullptr) noexcept {
        if (!active_ || !database_) {
            return ESDB_ERR_INVALID_STATE;
        }
        const Status status = esdb_savepoint_release(database_, name_, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            active_ = false;
            database_ = nullptr;
        }
        return status;
    }

    Status rollback(Error *error = nullptr) noexcept {
        if (!active_ || !database_) {
            return ESDB_ERR_INVALID_STATE;
        }
        const Status status = esdb_savepoint_rollback(database_, name_, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            active_ = false;
            database_ = nullptr;
        }
        return status;
    }

    void reset() noexcept {
        if (active_ && database_) {
            esdb_error ignored;
            esdb_savepoint_rollback(database_, name_, &ignored);
        }
        active_ = false;
        database_ = nullptr;
    }

private:
    friend class Database;

    esdb_database *database_ = nullptr;
    bool active_ = false;
    char name_[ESDB_SAVEPOINT_NAME_MAX_BYTES + 1u]{};
};

/*
 * Owned database connection. Move-only; destroying the handle closes the
 * connection. Child handles (transactions, savepoints, subscriptions) must be
 * destroyed before the Database.
 */
class Database final {
public:
    Database() noexcept = default;

    explicit Database(esdb_database *native) noexcept
        : native_(native) {}

    ~Database() noexcept {
        reset();
    }

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    Database(Database &&other) noexcept
        : native_(other.release()) {}

    Database &operator=(Database &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ESDB_NODISCARD explicit operator bool() const noexcept {
        return native_ != nullptr;
    }

    ESDB_NODISCARD esdb_database *native_handle() const noexcept {
        return native_;
    }

    esdb_database *release() noexcept {
        esdb_database *result = native_;
        native_ = nullptr;
        return result;
    }

    void reset(esdb_database *replacement = nullptr) noexcept {
        if (native_) {
            esdb_close(native_);
        }
        native_ = replacement;
    }

    /*
     * A failed open leaves the existing handle untouched; success atomically
     * replaces ownership.
     */
    static Status open(
        const char *path_utf8,
        const esdb_open_options *options,
        Database &out,
        Error *error = nullptr) noexcept {
        esdb_database *native = nullptr;
        const Status status = esdb_open(path_utf8, options, &native, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            out.reset(native);
        }
        return status;
    }

    static Status backend_capabilities(esdb_backend_capabilities &out, Error *error = nullptr) noexcept {
        std::memset(&out, 0, sizeof(out));
        out.struct_size = sizeof(out);
        return esdb_backend_capabilities_get(&out, error ? error->output() : nullptr);
    }

    Status exec(const char *sql_utf8, Error *error = nullptr) noexcept {
        return esdb_exec(native_, sql_utf8, error ? error->output() : nullptr);
    }

    Status begin(esdb_transaction_mode mode, Transaction &out, Error *error = nullptr) noexcept {
        esdb_transaction *native = nullptr;
        const Status status = esdb_begin(native_, mode, &native, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            out.reset(native);
        }
        return status;
    }

    Status savepoint_begin(const char *name, Error *error = nullptr) noexcept {
        return esdb_savepoint_begin(native_, name, error ? error->output() : nullptr);
    }

    Status savepoint_release(const char *name, Error *error = nullptr) noexcept {
        return esdb_savepoint_release(native_, name, error ? error->output() : nullptr);
    }

    Status savepoint_rollback(const char *name, Error *error = nullptr) noexcept {
        return esdb_savepoint_rollback(native_, name, error ? error->output() : nullptr);
    }

    ESDB_NODISCARD std::uint32_t savepoint_depth() const noexcept {
        return esdb_savepoint_depth(native_);
    }

    Status user_version_get(std::uint32_t &out, Error *error = nullptr) const noexcept {
        return esdb_user_version_get(native_, &out, error ? error->output() : nullptr);
    }

    Status user_version_set(std::uint32_t version, Error *error = nullptr) noexcept {
        return esdb_user_version_set(native_, version, error ? error->output() : nullptr);
    }

    Status migrate(
        std::uint32_t target_version,
        const esdb_migration *migrations,
        std::uint32_t migration_count,
        Error *error = nullptr) noexcept {
        return esdb_migrate(native_, target_version, migrations, migration_count, error ? error->output() : nullptr);
    }

    Status integrity_check(bool quick, Error *error = nullptr) noexcept {
        return esdb_integrity_check(native_, quick ? 1 : 0, error ? error->output() : nullptr);
    }

    Status backup_to(const char *target_path_utf8, Error *error = nullptr) noexcept {
        return esdb_backup_to(native_, target_path_utf8, error ? error->output() : nullptr);
    }

    Status health(esdb_database_health &out, Error *error = nullptr) const noexcept {
        std::memset(&out, 0, sizeof(out));
        out.struct_size = sizeof(out);
        return esdb_database_health_get(native_, &out, error ? error->output() : nullptr);
    }

    Status data_version_get(std::uint32_t &out, Error *error = nullptr) const noexcept {
        return esdb_data_version_get(native_, &out, error ? error->output() : nullptr);
    }

    ESDB_NODISCARD void *native_sqlite_handle() const noexcept {
        return esdb_native_handle(native_);
    }

    /* ---- durable ObjectStore convenience (optional Runtime layer) ---- */

    Status object_store_ensure(const char *store_name_utf8, Error *error = nullptr) noexcept {
        return esdb_object_store_ensure(native_, store_name_utf8, error ? error->output() : nullptr);
    }

    Status object_store_put(
        const char *store_name_utf8,
        const char *key_utf8,
        const Value &value,
        std::uint64_t *out_revision = nullptr,
        Error *error = nullptr) noexcept {
        return esdb_object_store_put(
            native_, store_name_utf8, key_utf8, value.native_handle(), out_revision,
            error ? error->output() : nullptr);
    }

    Status object_store_get(
        const char *store_name_utf8,
        const char *key_utf8,
        Value &out,
        Error *error = nullptr) noexcept {
        esdb_value *native = nullptr;
        const Status status = esdb_object_store_get(
            native_, store_name_utf8, key_utf8, &native, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            out.reset(native);
        }
        return status;
    }

    Status object_store_erase(
        const char *store_name_utf8,
        const char *key_utf8,
        bool *out_deleted = nullptr,
        std::uint64_t *out_revision = nullptr,
        Error *error = nullptr) noexcept {
        int deleted = 0;
        const Status status = esdb_object_store_delete(
            native_, store_name_utf8, key_utf8, &deleted, out_revision,
            error ? error->output() : nullptr);
        if (status == ESDB_OK && out_deleted) {
            *out_deleted = deleted != 0;
        }
        return status;
    }

    Status object_store_exists(
        const char *store_name_utf8,
        const char *key_utf8,
        bool &out_exists,
        Error *error = nullptr) noexcept {
        int exists = 0;
        const Status status = esdb_object_store_exists(
            native_, store_name_utf8, key_utf8, &exists, error ? error->output() : nullptr);
        if (status == ESDB_OK) {
            out_exists = exists != 0;
        }
        return status;
    }

    Status object_store_count(const char *store_name_utf8, std::uint64_t &out_count, Error *error = nullptr) noexcept {
        return esdb_object_store_count(native_, store_name_utf8, &out_count, error ? error->output() : nullptr);
    }

    Status object_store_scan(
        const char *store_name_utf8,
        const char *after_key_or_null_utf8,
        std::uint32_t limit,
        esdb_object_record_callback callback,
        void *user_data,
        std::uint32_t *out_record_count = nullptr,
        Error *error = nullptr) noexcept {
        return esdb_object_store_scan(
            native_,
            store_name_utf8,
            after_key_or_null_utf8,
            limit,
            callback,
            user_data,
            out_record_count,
            error ? error->output() : nullptr);
    }

    Status object_store_revision(std::uint64_t &out_revision, Error *error = nullptr) const noexcept {
        return esdb_object_store_revision(native_, &out_revision, error ? error->output() : nullptr);
    }

private:
    friend class Savepoint;

    esdb_database *native_ = nullptr;
};

inline Status Savepoint::begin(Database &database, const char *name, Savepoint &out, Error *error) noexcept {
    if (!database || !name) {
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    const std::size_t length = std::strlen(name);
    if (length == 0u || length > ESDB_SAVEPOINT_NAME_MAX_BYTES) {
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    const Status status = esdb_savepoint_begin(database.native_handle(), name, error ? error->output() : nullptr);
    if (status != ESDB_OK) {
        return status;
    }
    out.reset();
    out.database_ = database.native_handle();
    out.active_ = true;
    std::memcpy(out.name_, name, length + 1u);
    return ESDB_OK;
}

}  // namespace esdb

#undef ESDB_NODISCARD

#endif /* ESDB_ESDB_HPP */
