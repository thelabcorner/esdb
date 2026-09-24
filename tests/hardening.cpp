#include <esdb/esdb.h>
#include <esdb/esdb_store.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;

#define CHECK(expr) do {     if (!(expr)) {         std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__);         ++failures;     } } while (0)

static void cleanup_db(const std::string &path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

static esdb_status migration_create_a(
    esdb_database *db, uint32_t from, uint32_t to, void *, esdb_error *error) {
    CHECK(from == 0u);
    CHECK(to == 1u);
    return esdb_exec(db, "CREATE TABLE migration_a(id INTEGER PRIMARY KEY);", error);
}

static esdb_status migration_fail_b(
    esdb_database *db, uint32_t from, uint32_t to, void *, esdb_error *error) {
    CHECK(from == 1u);
    CHECK(to == 2u);
    const esdb_status status =
        esdb_exec(db, "CREATE TABLE migration_b(id INTEGER PRIMARY KEY);", error);
    if (status != ESDB_OK) return status;

    /* Deliberately violate the callback-side error/status pairing. */
    if (error) {
        esdb_error_clear(error);
        error->status = ESDB_ERR_CONSTRAINT;
        error->phase = ESDB_PHASE_EXEC;
        error->sqlite_code = 19;
        error->sqlite_extended_code = 19;
        std::snprintf(
            error->message, sizeof(error->message),
            "%s", "callback-provided migration diagnostic");
    }
    return ESDB_ERR_MIGRATION;
}

struct ChangeCount {
    uint32_t count = 0u;
    uint64_t last = 0u;
};

static int count_change(const esdb_change *change, void *user) {
    auto *capture = static_cast<ChangeCount *>(user);
    CHECK(change != nullptr);
    ++capture->count;
    capture->last = change->revision;
    return 0;
}

static int throwing_change(const esdb_change *, void *) {
    throw std::runtime_error("intentional callback fault");
}

static void check_value_roundtrip(
    esdb_database *db,
    const char *key,
    esdb_value *value,
    esdb_value_type expected,
    esdb_error *error) {
    uint64_t revision = 0u;
    CHECK(esdb_store_put(db, "typed", key, value, &revision, error) == ESDB_OK);
    CHECK(revision > 0u);
    esdb_value *loaded = nullptr;
    CHECK(esdb_store_get(db, "typed", key, &loaded, error) == ESDB_OK);
    CHECK(loaded != nullptr);
    if (loaded) CHECK(esdb_value_type_of(loaded) == expected);
    esdb_value_destroy(loaded);
}

int main() {
    const std::string path = "esdb-hardening.sqlite";
    const std::string migration_path = "esdb-hardening-migration.sqlite";
    cleanup_db(path);
    cleanup_db(migration_path);

    esdb_error error{};
    esdb_database *invalid = nullptr;
    CHECK(esdb_open(nullptr, nullptr, &invalid, &error) == ESDB_ERR_INVALID_ARGUMENT);
    CHECK(error.status == ESDB_ERR_INVALID_ARGUMENT);
    CHECK(error.phase == ESDB_PHASE_OPEN);
    CHECK(error.message[0] != '\0');

    esdb_backend_capabilities bad_capabilities{};
    bad_capabilities.struct_size = 1u;
    CHECK(esdb_backend_capabilities_get(&bad_capabilities, &error) ==
          ESDB_ERR_INVALID_ARGUMENT);
    CHECK(error.status == ESDB_ERR_INVALID_ARGUMENT);

    esdb_backend_capabilities capabilities{};
    CHECK(esdb_backend_capabilities_get(&capabilities, &error) == ESDB_OK);
    CHECK(error.status == ESDB_OK);
    CHECK(std::strcmp(capabilities.backend_id, "sqlite") == 0);
    CHECK(capabilities.supports_wal == 1u);
    CHECK(capabilities.supports_multiprocess == 1u);
    CHECK(capabilities.supports_stock_tools == 1u);
    CHECK(capabilities.supports_backup == 1u);
    CHECK(capabilities.compression_supported == 0u);
    CHECK(capabilities.page_codec == ESDB_CODEC_NONE);

    esdb_open_options options{};
    esdb_open_options_init(&options);
    options.journal_mode = ESDB_JOURNAL_WAL;
    options.synchronous = ESDB_SYNCHRONOUS_FULL;

    esdb_open_options bad_options = options;
    bad_options.flags |= (1u << 31);
    CHECK(esdb_open(":memory:", &bad_options, &invalid, &error) ==
          ESDB_ERR_INVALID_ARGUMENT);
    CHECK(invalid == nullptr);

    bad_options = options;
    bad_options.foreign_keys = 2u;
    CHECK(esdb_open(":memory:", &bad_options, &invalid, &error) ==
          ESDB_ERR_INVALID_ARGUMENT);
    CHECK(invalid == nullptr);

    bad_options = options;
    bad_options.reserved[0] = 1u;
    CHECK(esdb_open(":memory:", &bad_options, &invalid, &error) ==
          ESDB_ERR_INVALID_ARGUMENT);
    CHECK(invalid == nullptr);

    esdb_database *db = nullptr;
    CHECK(esdb_open(path.c_str(), &options, &db, &error) == ESDB_OK);
    if (!db) return 1;

    /* Store failures participate in Runtime health/observability. */
    esdb_database_health health_before{};
    health_before.struct_size = sizeof(health_before);
    CHECK(esdb_database_health_get(db, &health_before, &error) == ESDB_OK);
    uint64_t ignored_count = 0u;
    CHECK(esdb_store_count(db, "", &ignored_count, &error) == ESDB_ERR_INVALID_ARGUMENT);
    esdb_database_health health_after{};
    health_after.struct_size = sizeof(health_after);
    CHECK(esdb_database_health_get(db, &health_after, &error) == ESDB_OK);
    CHECK(health_after.operation_count > health_before.operation_count);
    CHECK(health_after.error_count > health_before.error_count);
    CHECK(health_after.last_error_status == ESDB_ERR_INVALID_ARGUMENT);
    CHECK(health_after.last_error_phase == ESDB_PHASE_STORE);

    /* Transaction + LIFO savepoint semantics. */
    esdb_transaction *tx = nullptr;
    CHECK(esdb_begin(db, ESDB_TRANSACTION_IMMEDIATE, &tx, &error) == ESDB_OK);
    esdb_value *one = nullptr;
    CHECK(esdb_value_create_int32(1, &one, &error) == ESDB_OK);
    CHECK(esdb_store_put(db, "tx", "keep", one, nullptr, &error) == ESDB_OK);
    CHECK(esdb_savepoint_begin(db, "sp1", &error) == ESDB_OK);
    CHECK(esdb_savepoint_depth(db) == 1u);
    CHECK(esdb_store_put(db, "tx", "rollback", one, nullptr, &error) == ESDB_OK);
    CHECK(esdb_savepoint_rollback(db, "sp1", &error) == ESDB_OK);
    CHECK(esdb_savepoint_depth(db) == 0u);
    int exists = 0;
    CHECK(esdb_store_exists(db, "tx", "rollback", &exists, &error) == ESDB_OK);
    CHECK(exists == 0);
    CHECK(esdb_commit(tx, &error) == ESDB_OK);
    esdb_transaction_destroy(tx);
    tx = nullptr;
    CHECK(esdb_store_exists(db, "tx", "keep", &exists, &error) == ESDB_OK);
    CHECK(exists == 1);

    /* Raw SQL transaction control keeps ESDB's transaction handle truthful. */
    CHECK(esdb_begin(db, ESDB_TRANSACTION_IMMEDIATE, &tx, &error) == ESDB_OK);
    CHECK(esdb_transaction_active(tx) == 1);
    CHECK(esdb_exec(db, "COMMIT;", &error) == ESDB_OK);
    CHECK(esdb_transaction_active(tx) == 0);
    CHECK(esdb_commit(tx, &error) == ESDB_ERR_INVALID_STATE);
    esdb_transaction_destroy(tx);
    tx = nullptr;

    CHECK(esdb_exec(db, "BEGIN;", &error) == ESDB_OK);
    esdb_database_health raw_tx_health{};
    raw_tx_health.struct_size = sizeof(raw_tx_health);
    CHECK(esdb_database_health_get(db, &raw_tx_health, &error) == ESDB_OK);
    CHECK(raw_tx_health.in_transaction == 1u);
    CHECK(esdb_begin(db, ESDB_TRANSACTION_IMMEDIATE, &tx, &error) ==
          ESDB_ERR_INVALID_STATE);
    CHECK(tx == nullptr);
    CHECK(esdb_exec(db, "ROLLBACK;", &error) == ESDB_OK);

    /* Store revisions and rows roll back with an outer transaction. */
    uint64_t before_outer_rollback = 0u;
    CHECK(esdb_store_revision(db, &before_outer_rollback, &error) == ESDB_OK);
    CHECK(esdb_begin(db, ESDB_TRANSACTION_IMMEDIATE, &tx, &error) == ESDB_OK);
    uint64_t provisional_revision = 0u;
    CHECK(esdb_store_put(
        db, "tx", "outer-rollback", one, &provisional_revision, &error) == ESDB_OK);
    CHECK(provisional_revision > before_outer_rollback);
    uint64_t in_outer_revision = 0u;
    CHECK(esdb_store_revision(db, &in_outer_revision, &error) == ESDB_OK);
    CHECK(in_outer_revision == provisional_revision);
    CHECK(esdb_rollback(tx, &error) == ESDB_OK);
    esdb_transaction_destroy(tx);
    tx = nullptr;
    CHECK(esdb_store_exists(db, "tx", "outer-rollback", &exists, &error) == ESDB_OK);
    CHECK(exists == 0);
    uint64_t after_outer_rollback = 0u;
    CHECK(esdb_store_revision(db, &after_outer_rollback, &error) == ESDB_OK);
    CHECK(after_outer_rollback == before_outer_rollback);

    esdb_value_destroy(one);

    /* Canonical value tags survive Store round-trips. */
    esdb_value *value = nullptr;
    CHECK(esdb_value_create_null(&value, &error) == ESDB_OK);
    check_value_roundtrip(db, "null", value, ESDB_VALUE_NULL, &error);
    esdb_value_destroy(value);

    CHECK(esdb_value_create_bool(1, &value, &error) == ESDB_OK);
    check_value_roundtrip(db, "bool", value, ESDB_VALUE_BOOL, &error);
    esdb_value_destroy(value);

    CHECK(esdb_value_create_int32(-1234567, &value, &error) == ESDB_OK);
    check_value_roundtrip(db, "i32", value, ESDB_VALUE_INT32, &error);
    esdb_value_destroy(value);

    const int64_t large_i64 = 9223372036854770000LL;
    CHECK(esdb_value_create_int64(large_i64, &value, &error) == ESDB_OK);
    CHECK(esdb_store_put(db, "typed", "i64", value, nullptr, &error) == ESDB_OK);
    esdb_value_destroy(value);
    value = nullptr;
    CHECK(esdb_store_get(db, "typed", "i64", &value, &error) == ESDB_OK);
    int64_t loaded_i64 = 0;
    CHECK(esdb_value_get_int64(value, &loaded_i64) == ESDB_OK);
    CHECK(loaded_i64 == large_i64);
    esdb_value_destroy(value);

    CHECK(esdb_value_create_double(3.141592653589793, &value, &error) == ESDB_OK);
    CHECK(esdb_store_put(db, "typed", "double", value, nullptr, &error) == ESDB_OK);
    esdb_value_destroy(value);
    value = nullptr;
    CHECK(esdb_store_get(db, "typed", "double", &value, &error) == ESDB_OK);
    double loaded_double = 0.0;
    CHECK(esdb_value_get_double(value, &loaded_double) == ESDB_OK);
    CHECK(std::fabs(loaded_double - 3.141592653589793) < 1e-15);
    esdb_value_destroy(value);

    const char utf8[] = "hello";
    CHECK(esdb_value_create_text(ESDB_VALUE_UTF8, utf8, 5u, &value, &error) == ESDB_OK);
    check_value_roundtrip(db, "utf8", value, ESDB_VALUE_UTF8, &error);
    esdb_value_destroy(value);

    CHECK(esdb_value_create_text(
        ESDB_VALUE_UTF8, nullptr, 0u, &value, &error) == ESDB_OK);
    check_value_roundtrip(db, "utf8-empty", value, ESDB_VALUE_UTF8, &error);
    esdb_value_destroy(value);

    CHECK(esdb_value_create_bytes(nullptr, 0u, &value, &error) == ESDB_OK);
    check_value_roundtrip(db, "bytes-empty", value, ESDB_VALUE_BYTES, &error);
    esdb_value_destroy(value);

    const unsigned char bytes[] = {0x00u, 0xffu, 0x10u, 0x80u};
    CHECK(esdb_value_create_bytes(bytes, sizeof(bytes), &value, &error) == ESDB_OK);
    CHECK(esdb_store_put(db, "typed", "bytes", value, nullptr, &error) == ESDB_OK);
    esdb_value_destroy(value);
    value = nullptr;
    CHECK(esdb_store_get(db, "typed", "bytes", &value, &error) == ESDB_OK);
    const void *loaded_bytes = nullptr;
    uint64_t loaded_size = 0u;
    CHECK(esdb_value_get_data(value, &loaded_bytes, &loaded_size) == ESDB_OK);
    CHECK(loaded_size == sizeof(bytes));
    CHECK(loaded_bytes && std::memcmp(loaded_bytes, bytes, sizeof(bytes)) == 0);
    esdb_value_destroy(value);

    const char object_text[] = "{\"a\":1}";
    CHECK(esdb_value_create_text(
        ESDB_VALUE_OBJECT, object_text, sizeof(object_text) - 1u, &value, &error) == ESDB_OK);
    check_value_roundtrip(db, "object", value, ESDB_VALUE_OBJECT, &error);
    esdb_value_destroy(value);

    const char array_text[] = "[1,2,3]";
    CHECK(esdb_value_create_text(
        ESDB_VALUE_ARRAY, array_text, sizeof(array_text) - 1u, &value, &error) == ESDB_OK);
    check_value_roundtrip(db, "array", value, ESDB_VALUE_ARRAY, &error);
    esdb_value_destroy(value);

    /* Revision is durable metadata, not the max surviving change row. */
    uint64_t before_prune = 0u;
    CHECK(esdb_store_revision(db, &before_prune, &error) == ESDB_OK);
    CHECK(before_prune > 0u);
    uint64_t pruned = 0u;
    CHECK(esdb_store_prune_changes(db, before_prune, &pruned, &error) == ESDB_OK);
    CHECK(pruned > 0u);
    uint64_t after_prune = 0u;
    CHECK(esdb_store_revision(db, &after_prune, &error) == ESDB_OK);
    CHECK(after_prune == before_prune);

    esdb_close(db);
    db = nullptr;
    CHECK(esdb_open(path.c_str(), &options, &db, &error) == ESDB_OK);
    CHECK(db != nullptr);
    uint64_t after_reopen = 0u;
    CHECK(esdb_store_revision(db, &after_reopen, &error) == ESDB_OK);
    CHECK(after_reopen == before_prune);

    CHECK(esdb_value_create_int32(7, &value, &error) == ESDB_OK);
    uint64_t rev_a = 0u;
    uint64_t rev_b = 0u;
    uint64_t rev_c = 0u;
    CHECK(esdb_store_put(db, "changes", "a", value, &rev_a, &error) == ESDB_OK);
    CHECK(esdb_store_put(db, "changes", "b", value, &rev_b, &error) == ESDB_OK);
    CHECK(esdb_store_put(db, "changes", "c", value, &rev_c, &error) == ESDB_OK);
    esdb_value_destroy(value);
    CHECK(rev_a > before_prune);
    CHECK(rev_b > rev_a);
    CHECK(rev_c > rev_b);

    /* Shared FULLMUTEX connections serialize complete Store mutations. */
    constexpr int thread_count = 8;
    constexpr int writes_per_thread = 32;
    constexpr int total_writes = thread_count * writes_per_thread;
    std::vector<uint64_t> concurrent_revisions(total_writes, 0u);
    std::atomic<int> concurrent_errors{0};
    CHECK(esdb_value_create_int32(99, &value, &error) == ESDB_OK);
    std::vector<std::thread> writers;
    writers.reserve(thread_count);
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        writers.emplace_back([&, thread_index]() {
            for (int item = 0; item < writes_per_thread; ++item) {
                char key[64]{};
                std::snprintf(key, sizeof(key), "t%02d-k%03d", thread_index, item);
                esdb_error thread_error{};
                uint64_t revision = 0u;
                const esdb_status status = esdb_store_put(
                    db, "concurrent", key, value, &revision, &thread_error);
                if (status != ESDB_OK || revision == 0u) {
                    concurrent_errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                concurrent_revisions[
                    thread_index * writes_per_thread + item] = revision;
            }
        });
    }
    for (auto &writer : writers) writer.join();
    esdb_value_destroy(value);
    value = nullptr;
    CHECK(concurrent_errors.load(std::memory_order_relaxed) == 0);
    std::sort(concurrent_revisions.begin(), concurrent_revisions.end());
    CHECK(concurrent_revisions.front() > rev_c);
    CHECK(std::adjacent_find(
        concurrent_revisions.begin(), concurrent_revisions.end()) ==
        concurrent_revisions.end());
    uint64_t concurrent_count = 0u;
    CHECK(esdb_store_count(
        db, "concurrent", &concurrent_count, &error) == ESDB_OK);
    CHECK(concurrent_count == static_cast<uint64_t>(total_writes));

    /* The public revision domain is SQLite's positive signed-64 rowid range. */
    const uint64_t invalid_revision = std::numeric_limits<uint64_t>::max();
    ChangeCount invalid_changes{};
    uint64_t invalid_last = 0u;
    uint32_t invalid_delivered = 0u;
    CHECK(esdb_store_changes_since(
        db, nullptr, invalid_revision, 1u, count_change, &invalid_changes,
        &invalid_last, &invalid_delivered, &error) == ESDB_ERR_INVALID_ARGUMENT);
    esdb_subscription *invalid_subscription = nullptr;
    CHECK(esdb_subscribe(
        db, nullptr, invalid_revision, &invalid_subscription, &error) ==
        ESDB_ERR_INVALID_ARGUMENT);
    CHECK(invalid_subscription == nullptr);
    uint64_t invalid_pruned = 0u;
    CHECK(esdb_store_prune_changes(
        db, invalid_revision, &invalid_pruned, &error) == ESDB_ERR_INVALID_ARGUMENT);

    /* limit==0 explicitly means the documented maximum. */
    ChangeCount all_changes{};
    uint64_t last = before_prune;
    uint32_t delivered = 0u;
    CHECK(esdb_store_changes_since(
        db, "changes", before_prune, 0u, count_change, &all_changes,
        &last, &delivered, &error) == ESDB_OK);
    CHECK(delivered == 3u);
    CHECK(all_changes.count == 3u);
    CHECK(last == rev_c);

    esdb_subscription *subscription = nullptr;
    CHECK(esdb_subscribe(db, "changes", rev_c, &subscription, &error) == ESDB_OK);
    CHECK(esdb_value_create_int32(8, &value, &error) == ESDB_OK);
    uint64_t rev_d = 0u;
    uint64_t rev_e = 0u;
    CHECK(esdb_store_put(db, "changes", "d", value, &rev_d, &error) == ESDB_OK);
    CHECK(esdb_store_put(db, "changes", "e", value, &rev_e, &error) == ESDB_OK);
    esdb_value_destroy(value);

    ChangeCount subscription_changes{};
    delivered = 0u;
    CHECK(esdb_subscription_poll(
        subscription, 0u, count_change, &subscription_changes, &delivered, &error) == ESDB_OK);
    CHECK(delivered == 2u);
    CHECK(subscription_changes.count == 2u);
    CHECK(esdb_subscription_revision(subscription) == rev_e);
    esdb_subscription_destroy(subscription);

    /* Throwing C++ callbacks are contained by the C ABI. */
    CHECK(esdb_store_changes_since(
        db, "changes", rev_c, 0u, throwing_change, nullptr,
        &last, &delivered, &error) == ESDB_ERR_INTERNAL);
    CHECK(error.status == ESDB_ERR_INTERNAL);
    CHECK(error.phase == ESDB_PHASE_SUBSCRIPTION);

    CHECK(esdb_integrity_check(db, 1, &error) == ESDB_OK);
    esdb_close(db);

    /* Migration failure rolls back all steps and user_version atomically. */
    esdb_database *migration_db = nullptr;
    CHECK(esdb_open(migration_path.c_str(), &options, &migration_db, &error) == ESDB_OK);
    esdb_migration migrations[] = {
        {0u, 1u, migration_create_a, nullptr},
        {1u, 2u, migration_fail_b, nullptr},
    };
    CHECK(esdb_migrate(migration_db, 2u, migrations, 2u, &error) == ESDB_ERR_MIGRATION);
    CHECK(error.status == ESDB_ERR_MIGRATION);
    CHECK(error.phase == ESDB_PHASE_MIGRATION);
    CHECK(error.sqlite_code == 19);
    CHECK(std::strcmp(
        error.message, "callback-provided migration diagnostic") == 0);
    uint32_t version = 99u;
    CHECK(esdb_user_version_get(migration_db, &version, &error) == ESDB_OK);
    CHECK(version == 0u);
    CHECK(esdb_exec(
        migration_db, "CREATE TABLE migration_a(id INTEGER PRIMARY KEY);", &error) == ESDB_OK);
    esdb_close(migration_db);

    cleanup_db(path);
    cleanup_db(migration_path);

    if (failures != 0) {
        std::fprintf(stderr, "%d ESDB hardening checks failed\n", failures);
        return 1;
    }
    std::puts("ESDB hardening: PASS");
    return 0;
}
