#ifndef ESDB_BENCH_ORM_COMMON_HPP_INCLUDED
#define ESDB_BENCH_ORM_COMMON_HPP_INCLUDED

#include <esdb/esdb.h>

#if defined(__has_include)
#  if __has_include(<esdb/sqlite3.h>)
#    include <esdb/sqlite3.h>
#  else
#    include <sqlite3.h>
#  endif
#else
#  include <sqlite3.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace esdb_orm_bench {

static const int kRows = 10000;
static const int kWarmupReads = 2000;
static const int kWarmupWrites = 1000;
static const int kReadOps = 50000;
static const int kPrepareEachReadOps = 10000;
static const int kWriteOps = 10000;
static const int kIterations = 7;
static const int kScanIterations = 15;
static volatile std::uint32_t g_decoded_row_checksum = 0u;

struct Input {
    std::int64_t id;
    std::string name;
    std::string email;
    std::int64_t created_at;
};

struct DecodedRow {
    std::int64_t id;
    std::string name;
    std::string email;
    std::int64_t created_at;
};

struct Result {
    const char *implementation;
    double startup_ns;
    double prepare_ns;
    double prepare_per_statement_ns;
    double prepare_each_point_read_ns;
    double point_read_ns;
    double point_write_ns;
    double scan_10k_ns;
    std::vector<double> scan_samples_ns;
    double bulk_insert_rows_per_second;
    sqlite3_int64 read_peak_memory_bytes;
    sqlite3_int64 write_peak_memory_bytes;
    sqlite3_int64 scan_peak_memory_bytes;
    sqlite3_int64 bulk_peak_memory_bytes;
    sqlite3_int64 read_peak_allocation_count;
    sqlite3_int64 write_peak_allocation_count;
    sqlite3_int64 scan_peak_allocation_count;
    sqlite3_int64 bulk_peak_allocation_count;
    sqlite3_int64 statement_memory_bytes;
    int statement_count;
    bool statement_reuse;
    std::uint32_t decoded_row_checksum;
};

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <typename Row>
inline std::uint32_t checksum_row(const Row &row) {
    std::uint32_t hash = 2166136261u;
    const std::uint64_t integers[2] = {
        static_cast<std::uint64_t>(row.id),
        static_cast<std::uint64_t>(row.created_at)
    };
    for (std::size_t field = 0u; field < 2u; ++field) {
        hash = (hash ^ static_cast<std::uint32_t>(integers[field])) * 16777619u;
        hash = (hash ^ static_cast<std::uint32_t>(integers[field] >> 32u)) * 16777619u;
    }
    const std::string *strings[2] = {&row.name, &row.email};
    for (std::size_t field = 0u; field < 2u; ++field) {
        hash = (hash ^ static_cast<std::uint32_t>(strings[field]->size())) * 16777619u;
        for (std::size_t index = 0u; index < strings[field]->size(); ++index) {
            hash = (hash ^ static_cast<unsigned char>((*strings[field])[index])) * 16777619u;
        }
    }
    return hash;
}

template <typename Row>
inline void observe_row(const Row &row) {
    g_decoded_row_checksum =
        (g_decoded_row_checksum ^ checksum_row(row)) * 16777619u;
}

template <typename Rows>
inline void observe_rows(const Rows &rows) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t index = 0u; index < rows.size(); ++index) {
        hash = (hash ^ checksum_row(rows[index])) * 16777619u;
    }
    g_decoded_row_checksum = (g_decoded_row_checksum ^ hash) * 16777619u;
}

inline double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n == 0u ? 0.0 :
        (n % 2u == 0u ? (values[n / 2u - 1u] + values[n / 2u]) * 0.5 : values[n / 2u]);
}

inline sqlite3 *native_handle(esdb_database *database) {
    return static_cast<sqlite3 *>(esdb_native_handle(database));
}

inline bool begin_immediate(esdb_database *database, esdb_transaction **out) {
    esdb_error error;
    esdb_error_clear(&error);
    return esdb_begin(database, ESDB_TRANSACTION_IMMEDIATE, out, &error) == ESDB_OK;
}

inline bool commit_transaction(esdb_transaction *transaction) {
    esdb_error error;
    esdb_error_clear(&error);
    const esdb_status status = esdb_commit(transaction, &error);
    esdb_transaction_destroy(transaction);
    return status == ESDB_OK;
}

inline void rollback_transaction(esdb_transaction *transaction) {
    if (transaction == NULL) return;
    esdb_error error;
    esdb_error_clear(&error);
    (void)esdb_rollback(transaction, &error);
    esdb_transaction_destroy(transaction);
}

inline bool clear_user_table(sqlite3 *db) {
    return sqlite3_exec(db, "DELETE FROM \"user\";", NULL, NULL, NULL) == SQLITE_OK;
}

inline Input make_input(int index) {
    Input value;
    value.id = static_cast<std::int64_t>(index + 1);
    value.name = std::string("name-") + std::to_string(index + 1);
    value.email = std::string("user-") + std::to_string(index + 1) + "@example.com";
    value.created_at = static_cast<std::int64_t>(1735689600LL + index);
    return value;
}

template <typename Adapter>
std::vector<typename Adapter::InputType> make_inputs() {
    std::vector<typename Adapter::InputType> inputs;
    inputs.reserve(kRows);
    for (int i = 0; i < kRows; ++i) {
        inputs.push_back(Adapter::make_input(i));
    }
    return inputs;
}

inline int statement_count(sqlite3 *db) {
    int count = 0;
    for (sqlite3_stmt *stmt = sqlite3_next_stmt(db, NULL); stmt != NULL; stmt = sqlite3_next_stmt(db, stmt)) {
        ++count;
    }
    return count;
}

inline sqlite3_int64 statement_memory(sqlite3 *db) {
    sqlite3_int64 total = 0;
    for (sqlite3_stmt *stmt = sqlite3_next_stmt(db, NULL); stmt != NULL; stmt = sqlite3_next_stmt(db, stmt)) {
        total += static_cast<sqlite3_int64>(sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_MEMUSED, 0));
    }
    return total;
}

inline void reset_memory_highwater() {
    sqlite3_int64 current = 0;
    sqlite3_int64 highwater = 0;
    (void)sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &current, &highwater, 1);
}

inline sqlite3_int64 memory_peak_delta() {
    sqlite3_int64 current = 0;
    sqlite3_int64 highwater = 0;
    (void)sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &current, &highwater, 0);
    return highwater > current ? highwater - current : 0;
}

inline void reset_allocation_highwater() {
    sqlite3_int64 current = 0;
    sqlite3_int64 highwater = 0;
    (void)sqlite3_status64(SQLITE_STATUS_MALLOC_COUNT, &current, &highwater, 1);
}

inline sqlite3_int64 allocation_peak_delta() {
    sqlite3_int64 current = 0;
    sqlite3_int64 highwater = 0;
    (void)sqlite3_status64(SQLITE_STATUS_MALLOC_COUNT, &current, &highwater, 0);
    return highwater > current ? highwater - current : 0;
}

inline bool read_sqlite_text(sqlite3_stmt *statement, int column, std::string &out) {
    const unsigned char *text = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (bytes < 0 || (!text && bytes > 0)) return false;
    if (bytes == 0) {
        out.clear();
    } else {
        out.assign(
            reinterpret_cast<const char *>(text),
            static_cast<std::size_t>(bytes));
    }
    return true;
}

inline bool prepare_each_find_once(sqlite3 *db, std::int64_t id, DecodedRow *out) {
    static const char sql[] =
        "SELECT \"id\",\"name\",\"email\",\"created_at\" FROM \"user\" WHERE \"id\" = ?";
    if (db == NULL || out == NULL) return false;
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v3(db, sql, -1, 0u, &statement, NULL);
    if (rc != SQLITE_OK || statement == NULL) {
        sqlite3_finalize(statement);
        return false;
    }

    bool ok = sqlite3_bind_parameter_count(statement) == 1 &&
              sqlite3_column_count(statement) == 4;
    if (ok) rc = sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(id));
    else rc = SQLITE_MISMATCH;
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) {
        out->id = static_cast<std::int64_t>(sqlite3_column_int64(statement, 0));
        ok = read_sqlite_text(statement, 1, out->name) &&
             read_sqlite_text(statement, 2, out->email);
        out->created_at = static_cast<std::int64_t>(sqlite3_column_int64(statement, 3));
    } else {
        ok = false;
    }
    const int finalize_rc = sqlite3_finalize(statement);
    return ok && finalize_rc == SQLITE_OK;
}

inline double benchmark_prepare_each_point_read(sqlite3 *db) {
    DecodedRow row;
    for (int i = 0; i < 500; ++i) {
        if (!prepare_each_find_once(
                db, static_cast<std::int64_t>((i % kRows) + 1), &row)) return -1.0;
        observe_row(row);
    }
    std::vector<double> samples;
    samples.reserve(kIterations);
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t start = now_ns();
        for (int i = 0; i < kPrepareEachReadOps; ++i) {
            if (!prepare_each_find_once(
                    db, static_cast<std::int64_t>((i % kRows) + 1), &row)) return -1.0;
            observe_row(row);
        }
        const std::uint64_t end = now_ns();
        samples.push_back(
            static_cast<double>(end - start) / static_cast<double>(kPrepareEachReadOps));
    }
    return median(samples);
}

template <typename Adapter>
bool seed(esdb_database *database, Adapter &adapter, const std::vector<typename Adapter::InputType> &inputs) {
    sqlite3 *db = native_handle(database);
    if (db == NULL || !clear_user_table(db)) return false;
    esdb_transaction *transaction = NULL;
    if (!begin_immediate(database, &transaction)) return false;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (adapter.insert(inputs[i]) != 1) {
            rollback_transaction(transaction);
            return false;
        }
    }
    return commit_transaction(transaction);
}

template <typename Adapter>
bool warmup(esdb_database *database, Adapter &adapter) {
    typename Adapter::Row row;
    for (int i = 0; i < kWarmupReads; ++i) {
        if (adapter.find(static_cast<std::int64_t>((i % kRows) + 1), &row) != 1) return false;
    }
    esdb_transaction *transaction = NULL;
    if (!begin_immediate(database, &transaction)) return false;
    for (int i = 0; i < kWarmupWrites; ++i) {
        if (adapter.update_name(1, (i & 1) ? "warm-a" : "warm-b") != 1) {
            rollback_transaction(transaction);
            return false;
        }
    }
    if (!commit_transaction(transaction)) return false;
    std::vector<typename Adapter::Row> rows;
    rows.reserve(kRows);
    return adapter.list(kRows, 0, &rows) == kRows && rows.size() == static_cast<std::size_t>(kRows);
}

template <typename Adapter>
Result run(const char *implementation, esdb_database *database, double startup_ns) {
    g_decoded_row_checksum = 0u;
    const std::vector<typename Adapter::InputType> inputs = make_inputs<Adapter>();

    const std::uint64_t prepare_start = now_ns();
    Adapter adapter(database);
    const bool prepared = adapter.valid();
    const std::uint64_t prepare_end = now_ns();
    if (!prepared) {
        std::fprintf(stderr, "%s adapter preparation failed\n", implementation);
        std::exit(2);
    }

    if (!seed(database, adapter, inputs)) {
        std::fprintf(stderr, "%s seed failed\n", implementation);
        std::exit(3);
    }
    if (!warmup(database, adapter)) {
        std::fprintf(stderr, "%s warmup failed\n", implementation);
        std::exit(4);
    }

    sqlite3 *db = native_handle(database);
    const double prepare_each_point_read = benchmark_prepare_each_point_read(db);
    if (prepare_each_point_read < 0.0) {
        std::fprintf(stderr, "%s prepare-each point-read control failed\n", implementation);
        std::exit(16);
    }
    const int statements_before = statement_count(db);
    const sqlite3_int64 statement_bytes = statement_memory(db);

    std::vector<double> read_samples;
    std::vector<double> write_samples;
    std::vector<double> scan_samples;
    std::vector<double> bulk_samples;
    read_samples.reserve(kIterations);
    write_samples.reserve(kIterations);
    scan_samples.reserve(kScanIterations);
    bulk_samples.reserve(kIterations);

    sqlite3_int64 read_peak = 0;
    sqlite3_int64 write_peak = 0;
    sqlite3_int64 scan_peak = 0;
    sqlite3_int64 bulk_peak = 0;
    sqlite3_int64 read_alloc_peak = 0;
    sqlite3_int64 write_alloc_peak = 0;
    sqlite3_int64 scan_alloc_peak = 0;
    sqlite3_int64 bulk_alloc_peak = 0;

    typename Adapter::Row row;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        reset_memory_highwater();
        reset_allocation_highwater();
        const std::uint64_t start = now_ns();
        for (int i = 0; i < kReadOps; ++i) {
            const std::int64_t id = static_cast<std::int64_t>((i % kRows) + 1);
            if (adapter.find(id, &row) != 1) std::exit(5);
            observe_row(row);
        }
        const std::uint64_t end = now_ns();
        read_samples.push_back(static_cast<double>(end - start) / static_cast<double>(kReadOps));
        read_peak = (std::max)(read_peak, memory_peak_delta());
        read_alloc_peak = (std::max)(read_alloc_peak, allocation_peak_delta());
    }

    for (int iteration = 0; iteration < kIterations; ++iteration) {
        esdb_transaction *transaction = NULL;
        if (!begin_immediate(database, &transaction)) std::exit(6);
        reset_memory_highwater();
        reset_allocation_highwater();
        const std::uint64_t start = now_ns();
        for (int i = 0; i < kWriteOps; ++i) {
            if (adapter.update_name(1, (i & 1) ? "write-a" : "write-b") != 1) {
                rollback_transaction(transaction);
                std::exit(7);
            }
        }
        const std::uint64_t end = now_ns();
        if (!commit_transaction(transaction)) std::exit(8);
        write_samples.push_back(static_cast<double>(end - start) / static_cast<double>(kWriteOps));
        write_peak = (std::max)(write_peak, memory_peak_delta());
        write_alloc_peak = (std::max)(write_alloc_peak, allocation_peak_delta());
    }

    std::vector<typename Adapter::Row> rows;
    rows.reserve(kRows);
    for (int iteration = 0; iteration < kScanIterations; ++iteration) {
        rows.clear();
        reset_memory_highwater();
        reset_allocation_highwater();
        const std::uint64_t start = now_ns();
        const int count = adapter.list(kRows, 0, &rows);
        if (count != kRows || rows.size() != static_cast<std::size_t>(kRows)) std::exit(9);
        observe_rows(rows);
        const std::uint64_t end = now_ns();
        scan_samples.push_back(static_cast<double>(end - start));
        scan_peak = (std::max)(scan_peak, memory_peak_delta());
        scan_alloc_peak = (std::max)(scan_alloc_peak, allocation_peak_delta());
    }

    for (int iteration = 0; iteration < kIterations; ++iteration) {
        if (!clear_user_table(db)) std::exit(10);
        esdb_transaction *transaction = NULL;
        if (!begin_immediate(database, &transaction)) std::exit(11);
        reset_memory_highwater();
        reset_allocation_highwater();
        const std::uint64_t start = now_ns();
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (adapter.insert(inputs[i]) != 1) {
                rollback_transaction(transaction);
                std::exit(12);
            }
        }
        if (!commit_transaction(transaction)) std::exit(13);
        const std::uint64_t end = now_ns();
        const double seconds = static_cast<double>(end - start) / 1000000000.0;
        bulk_samples.push_back(static_cast<double>(kRows) / seconds);
        bulk_peak = (std::max)(bulk_peak, memory_peak_delta());
        bulk_alloc_peak = (std::max)(bulk_alloc_peak, allocation_peak_delta());
    }

    typename Adapter::Row reuse_row;
    if (adapter.find(1, &reuse_row) != 1) std::exit(14);
    const int statements_after = statement_count(db);

    Result result;
    result.implementation = implementation;
    result.startup_ns = startup_ns;
    result.prepare_ns = static_cast<double>(prepare_end - prepare_start);
    result.prepare_per_statement_ns =
        statements_before > 0 ? result.prepare_ns / static_cast<double>(statements_before) : 0.0;
    result.prepare_each_point_read_ns = prepare_each_point_read;
    result.point_read_ns = median(read_samples);
    result.point_write_ns = median(write_samples);
    result.scan_10k_ns = median(scan_samples);
    result.scan_samples_ns = scan_samples;
    result.bulk_insert_rows_per_second = median(bulk_samples);
    result.read_peak_memory_bytes = read_peak;
    result.write_peak_memory_bytes = write_peak;
    result.scan_peak_memory_bytes = scan_peak;
    result.bulk_peak_memory_bytes = bulk_peak;
    result.read_peak_allocation_count = read_alloc_peak;
    result.write_peak_allocation_count = write_alloc_peak;
    result.scan_peak_allocation_count = scan_alloc_peak;
    result.bulk_peak_allocation_count = bulk_alloc_peak;
    result.statement_memory_bytes = statement_bytes;
    result.statement_count = statements_before;
    result.statement_reuse = statements_before == statements_after;
    result.decoded_row_checksum = g_decoded_row_checksum;
    return result;
}

inline void print_json(const Result &r) {
    std::printf(
        "{\"implementation\":\"%s\","
        "\"decoded_row_checksum\":%u,"
        "\"startup_ns\":%.3f,"
        "\"prepare_ns\":%.3f,"
        "\"prepare_per_statement_ns\":%.3f,"
        "\"prepare_each_point_read_ns\":%.3f,"
        "\"point_read_ns\":%.3f,"
        "\"point_write_ns\":%.3f,"
        "\"scan_10k_ns\":%.3f,"
        "\"bulk_insert_rows_per_second\":%.3f,"
        "\"read_peak_memory_bytes\":%lld,"
        "\"write_peak_memory_bytes\":%lld,"
        "\"scan_peak_memory_bytes\":%lld,"
        "\"bulk_peak_memory_bytes\":%lld,"
        "\"read_peak_allocation_count\":%lld,"
        "\"write_peak_allocation_count\":%lld,"
        "\"scan_peak_allocation_count\":%lld,"
        "\"bulk_peak_allocation_count\":%lld,"
        "\"statement_memory_bytes\":%lld,"
        "\"statement_count\":%d,"
        "\"statement_reuse\":%s,"
        "\"scan_samples_ns\":[",
        r.implementation,
        static_cast<unsigned int>(r.decoded_row_checksum),
        r.startup_ns,
        r.prepare_ns,
        r.prepare_per_statement_ns,
        r.prepare_each_point_read_ns,
        r.point_read_ns,
        r.point_write_ns,
        r.scan_10k_ns,
        r.bulk_insert_rows_per_second,
        static_cast<long long>(r.read_peak_memory_bytes),
        static_cast<long long>(r.write_peak_memory_bytes),
        static_cast<long long>(r.scan_peak_memory_bytes),
        static_cast<long long>(r.bulk_peak_memory_bytes),
        static_cast<long long>(r.read_peak_allocation_count),
        static_cast<long long>(r.write_peak_allocation_count),
        static_cast<long long>(r.scan_peak_allocation_count),
        static_cast<long long>(r.bulk_peak_allocation_count),
        static_cast<long long>(r.statement_memory_bytes),
        r.statement_count,
        r.statement_reuse ? "true" : "false");
    for (std::size_t i = 0; i < r.scan_samples_ns.size(); ++i) {
        if (i != 0u) std::printf(",");
        std::printf("%.3f", r.scan_samples_ns[i]);
    }
    std::printf("]}\n");
}

}  // namespace esdb_orm_bench

#endif
