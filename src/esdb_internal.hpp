#ifndef ESDB_INTERNAL_HPP
#define ESDB_INTERNAL_HPP

#include <esdb/esdb.h>
#include <esdb/esdb_store.h>

#include "esdb_nothrow_mutex.hpp"
#include <sqlite3.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

/*
 * Internal representation. Nothing in this header is part of the public ABI:
 * public headers expose only opaque pointers.
 */

struct esdb_database {
    sqlite3 *handle = nullptr;
    esdb_detail::NoThrowMutex state_mutex;
    esdb_detail::NoThrowMutex error_mutex;
    esdb_detail::NoThrowMutex store_write_mutex;
    bool transaction_active = false;
    esdb_transaction *active_transaction = nullptr;
    std::array<std::array<char, ESDB_SAVEPOINT_NAME_MAX_BYTES + 1u>, 64u> savepoints{};
    std::uint32_t savepoint_depth = 0u;
    bool store_schema_ready = false;
    esdb_open_options options{};
    std::atomic<std::uint64_t> operation_count{0};
    std::atomic<std::uint64_t> error_count{0};
    std::atomic<std::uint64_t> busy_count{0};
    std::atomic<std::uint64_t> commit_count{0};
    std::atomic<std::uint64_t> rollback_count{0};
    esdb_error last_error{};
};

struct esdb_transaction {
    esdb_database *database = nullptr;
    bool active = false;
};

struct esdb_value {
    esdb_value_type type = ESDB_VALUE_NULL;
    std::vector<std::uint8_t> payload;
};

struct esdb_subscription {
    esdb_database *database = nullptr;
    char store_name[ESDB_STORE_NAME_MAX_BYTES + 1u]{};
    bool all_stores = true;
    std::uint64_t revision = 0;
};

namespace esdb_detail {

/* ---- error model ---- */

void clear_error(esdb_error *error) noexcept;

void set_error(
    esdb_error *error,
    esdb_status status,
    esdb_phase phase,
    sqlite3 *db,
    int sqlite_code,
    const char *message) noexcept;

/*
 * Sets the error, records it as the connection's last error, and bumps the
 * error/busy counters. Returns `status` so call sites read
 * `return fail(database, error, ...)`.
 */
esdb_status fail(
    esdb_database *database,
    esdb_error *error,
    esdb_status status,
    esdb_phase phase,
    sqlite3 *db,
    int sqlite_code,
    const char *message) noexcept;

esdb_status map_sqlite_status(int code) noexcept;

void count_operation(esdb_database *database) noexcept;
void count_commit(esdb_database *database) noexcept;
void count_rollback(esdb_database *database) noexcept;

/* ---- SQL helpers ---- */

esdb_status exec_sql(esdb_database *database, const char *sql, esdb_phase phase, esdb_error *error) noexcept;
esdb_status query_u64(esdb_database *database, const char *sql, std::uint64_t *out, esdb_phase phase, esdb_error *error) noexcept;
esdb_status query_i64(esdb_database *database, const char *sql, std::int64_t *out, esdb_phase phase, esdb_error *error) noexcept;

/*
 * Prepared-statement guard used by the Store and internal queries. The
 * statement is finalized on destruction; the owner must not keep it past the
 * database handle.
 */
class Statement final {
public:
    Statement() noexcept = default;
    ~Statement() noexcept;

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    sqlite3_stmt *get() const noexcept {
        return statement_;
    }

    bool valid() const noexcept {
        return statement_ != nullptr;
    }

    esdb_status prepare(esdb_database *database, const char *sql, esdb_phase phase, esdb_error *error) noexcept;
    esdb_status step(esdb_database *database, int *out_step, esdb_phase phase, esdb_error *error) noexcept;
    esdb_status bind_text(esdb_database *database, int index, const char *text, int size, esdb_phase phase, esdb_error *error) noexcept;
    esdb_status bind_blob(esdb_database *database, int index, const void *data, int size, esdb_phase phase, esdb_error *error) noexcept;
    esdb_status bind_blob64(esdb_database *database, int index, const void *data, std::uint64_t size, esdb_phase phase, esdb_error *error) noexcept;
    esdb_status bind_zeroblob(esdb_database *database, int index, int size, esdb_phase phase, esdb_error *error) noexcept;
    esdb_status bind_int64(esdb_database *database, int index, std::int64_t value, esdb_phase phase, esdb_error *error) noexcept;
    esdb_status bind_int(esdb_database *database, int index, int value, esdb_phase phase, esdb_error *error) noexcept;

private:
    void reset() noexcept;
    sqlite3_stmt *statement_ = nullptr;
};

/* ---- validation ---- */

bool valid_utf8(const char *text, std::uint64_t size) noexcept;
bool valid_c_string(const char *text, std::size_t max_bytes) noexcept;
bool valid_store_name(const char *text) noexcept;
bool valid_store_key(const char *text) noexcept;
bool valid_savepoint_name(const char *text) noexcept;
std::int64_t unix_time_ms() noexcept;

/* ---- Store internals ---- */

esdb_status ensure_store_schema(esdb_database *database, esdb_error *error) noexcept;
esdb_status store_changes_since_impl(
    esdb_database *database,
    const char *store_name_or_null,
    std::uint64_t after_revision,
    std::uint32_t limit,
    esdb_change_callback callback,
    void *user_data,
    std::uint64_t *out_last_revision,
    std::uint32_t *out_change_count,
    esdb_error *error) noexcept;

}  // namespace esdb_detail

#endif /* ESDB_INTERNAL_HPP */
