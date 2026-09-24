#include <esdb/esdb.h>
#include <esdb/esdb_store.h>

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); ++failures; } } while (0)

static esdb_status migration_0_to_1(esdb_database *db, uint32_t from, uint32_t to, void *, esdb_error *error) {
    CHECK(from == 0u);
    CHECK(to == 1u);
    return esdb_exec(db, "CREATE TABLE app_state(id INTEGER PRIMARY KEY, name TEXT NOT NULL);", error);
}

struct ChangeCapture {
    uint32_t count = 0;
    uint64_t last = 0;
};

static int capture_change(const esdb_change *change, void *user) {
    auto *capture = static_cast<ChangeCapture *>(user);
    CHECK(change != nullptr);
    CHECK(change->store_name != nullptr);
    CHECK(change->key != nullptr);
    ++capture->count;
    capture->last = change->revision;
    return 1;
}

int main() {
    const std::string path = "esdb-runtime-smoke.sqlite";
    const std::string backup = "esdb-runtime-smoke.backup.sqlite";
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
    std::remove(backup.c_str());

    esdb_error error{};
    esdb_open_options options{};
    esdb_open_options_init(&options);
    options.journal_mode = ESDB_JOURNAL_WAL;
    options.synchronous = ESDB_SYNCHRONOUS_FULL;
    options.cache_kib = 16384u;

    esdb_database *db = nullptr;
    CHECK(esdb_open(path.c_str(), &options, &db, &error) == ESDB_OK);
    if (!db) return 1;
    CHECK(std::strcmp(esdb_version(), "0.1.0") == 0);
    CHECK(std::strcmp(esdb_sqlite_version(), "3.53.4") == 0);
    CHECK(esdb_integrity_check(db, 1, &error) == ESDB_OK);

    esdb_migration migration{0u, 1u, migration_0_to_1, nullptr};
    CHECK(esdb_migrate(db, 1u, &migration, 1u, &error) == ESDB_OK);
    uint32_t user_version = 0;
    CHECK(esdb_user_version_get(db, &user_version, &error) == ESDB_OK);
    CHECK(user_version == 1u);

    esdb_value *theme = nullptr;
    const char *dark = "dark";
    CHECK(esdb_value_create_text(ESDB_VALUE_UTF8, dark, 4u, &theme, &error) == ESDB_OK);
    uint64_t rev1 = 0;
    CHECK(esdb_store_put(db, "settings", "theme", theme, &rev1, &error) == ESDB_OK);
    CHECK(rev1 > 0u);
    esdb_value_destroy(theme);

    esdb_value *read = nullptr;
    CHECK(esdb_store_get(db, "settings", "theme", &read, &error) == ESDB_OK);
    const void *payload = nullptr;
    uint64_t payload_size = 0;
    CHECK(esdb_value_type_of(read) == ESDB_VALUE_UTF8);
    CHECK(esdb_value_get_data(read, &payload, &payload_size) == ESDB_OK);
    CHECK(payload_size == 4u);
    CHECK(payload && std::memcmp(payload, "dark", 4) == 0);
    esdb_value_destroy(read);

    uint64_t count = 0;
    CHECK(esdb_store_count(db, "settings", &count, &error) == ESDB_OK);
    CHECK(count == 1u);

    int exists = 0;
    CHECK(esdb_store_exists(db, "settings", "theme", &exists, &error) == ESDB_OK);
    CHECK(exists == 1);

    esdb_subscription *subscription = nullptr;
    CHECK(esdb_subscribe(db, "settings", 0u, &subscription, &error) == ESDB_OK);
    ChangeCapture capture{};
    uint32_t polled = 0;
    CHECK(esdb_subscription_poll(subscription, 64u, capture_change, &capture, &polled, &error) == ESDB_OK);
    CHECK(polled == 1u);
    CHECK(capture.count == 1u);
    CHECK(esdb_subscription_revision(subscription) == rev1);

    int deleted = 0;
    uint64_t rev2 = 0;
    CHECK(esdb_store_delete(db, "settings", "theme", &deleted, &rev2, &error) == ESDB_OK);
    CHECK(deleted == 1);
    CHECK(rev2 > rev1);
    CHECK(esdb_subscription_poll(subscription, 64u, capture_change, &capture, &polled, &error) == ESDB_OK);
    CHECK(polled == 1u);
    CHECK(esdb_subscription_revision(subscription) == rev2);
    esdb_subscription_destroy(subscription);

    esdb_transaction *tx = nullptr;
    CHECK(esdb_begin(db, ESDB_TRANSACTION_IMMEDIATE, &tx, &error) == ESDB_OK);
    esdb_value *temp = nullptr;
    CHECK(esdb_value_create_int64(9223372036854770000LL, &temp, &error) == ESDB_OK);
    CHECK(esdb_store_put(db, "settings", "temporary", temp, nullptr, &error) == ESDB_OK);
    esdb_value_destroy(temp);
    CHECK(esdb_rollback(tx, &error) == ESDB_OK);
    esdb_transaction_destroy(tx);
    CHECK(esdb_store_exists(db, "settings", "temporary", &exists, &error) == ESDB_OK);
    CHECK(exists == 0);

    esdb_database_health health{};
    health.struct_size = sizeof(health);
    CHECK(esdb_database_health_get(db, &health, &error) == ESDB_OK);
    CHECK(health.page_size >= 512u);
    CHECK(health.page_count > 0u);
    CHECK(health.configured_cache_kib == 16384);
    CHECK(health.journal_mode == ESDB_JOURNAL_WAL);

    CHECK(esdb_backup_to(db, backup.c_str(), &error) == ESDB_OK);
    CHECK(esdb_integrity_check(db, 0, &error) == ESDB_OK);
    esdb_close(db);

    esdb_database *backup_db = nullptr;
    esdb_open_options readonly{};
    esdb_open_options_init(&readonly);
    readonly.flags = ESDB_OPEN_READONLY | ESDB_OPEN_FULLMUTEX;
    CHECK(esdb_open(backup.c_str(), &readonly, &backup_db, &error) == ESDB_OK);
    CHECK(esdb_integrity_check(backup_db, 1, &error) == ESDB_OK);
    esdb_close(backup_db);

    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
    std::remove(backup.c_str());

    if (failures != 0) {
        std::fprintf(stderr, "%d ESDB runtime smoke checks failed\n", failures);
        return 1;
    }
    std::puts("ESDB runtime smoke: PASS");
    return 0;
}
