#include <esdb/esdb.hpp>

#include <cstdio>
#include <cstring>

int main() {
    const char *path = "esdb-cpp-smoke.sqlite";
    std::remove(path);
    std::remove("esdb-cpp-smoke.sqlite-wal");
    std::remove("esdb-cpp-smoke.sqlite-shm");

    esdb_open_options options{};
    esdb_open_options_init(&options);
    options.journal_mode = ESDB_JOURNAL_WAL;

    esdb::Database db;
    esdb::Error error;
    if (esdb::Database::open(path, &options, db, &error) != ESDB_OK) return 1;

    esdb::Value text;
    if (esdb::Value::utf8("hello", 5u, text, &error) != ESDB_OK) return 2;
    const void *data = nullptr;
    std::uint64_t size = 0;
    if (text.data(&data, size, &error) != ESDB_OK) return 3;
    if (size != 5u || !data || std::memcmp(data, "hello", 5) != 0) return 4;

    esdb::Value exact;
    if (esdb::Value::int64(
            INT64_C(9007199254740993), exact, &error) != ESDB_OK) return 5;
    bool wrong_type = false;
    if (exact.get_bool(wrong_type, &error) != ESDB_ERR_TYPE_MISMATCH) return 6;
    if (error.status() != ESDB_ERR_TYPE_MISMATCH) return 7;
    std::int64_t recovered = 0;
    if (exact.get_int64(recovered, &error) != ESDB_OK) return 8;
    if (!error.ok()) return 9;
    if (recovered != INT64_C(9007199254740993)) return 10;

    esdb::Transaction tx;
    if (db.begin(ESDB_TRANSACTION_IMMEDIATE, tx, &error) != ESDB_OK) return 11;
    if (!tx.active()) return 12;

    esdb::Savepoint savepoint;
    if (esdb::Savepoint::begin(db, "cpp_probe", savepoint, &error) != ESDB_OK) return 13;
    if (!savepoint.active()) return 14;
    if (savepoint.release(&error) != ESDB_OK) return 15;
    if (tx.rollback(&error) != ESDB_OK) return 16;

    esdb_database_health health;
    std::memset(&health, 0xA5, sizeof(health));
    if (db.health(health, &error) != ESDB_OK) return 17;
    if (health.page_size < 512u) return 18;
    if (health.struct_size != sizeof(health)) return 19;

    esdb_backend_capabilities caps;
    std::memset(&caps, 0xA5, sizeof(caps));
    if (esdb::Database::backend_capabilities(caps, &error) != ESDB_OK) return 20;
    if (std::strcmp(caps.backend_id, "sqlite") != 0) return 21;
    if (caps.struct_size != sizeof(caps)) return 22;

    db.reset();
    std::remove(path);
    std::remove("esdb-cpp-smoke.sqlite-wal");
    std::remove("esdb-cpp-smoke.sqlite-shm");
    std::puts("ESDB C++ smoke: PASS");
    return 0;
}
