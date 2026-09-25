#include "esdb_externalobject_abi.h"

#include <esdb/esdb_store.h>
#include <esdb/esdb_object_store.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        ++failures; \
    } \
} while (0)

static esabi_value make_double(double value) {
    esabi_value result{};
    esabi_value_set_double(&result, value);
    return result;
}

static esabi_value make_i32(esabi_i32 value) {
    esabi_value result{};
    esabi_value_set_i32(&result, value);
    return result;
}

static esabi_value make_string(char *value) {
    esabi_value result{};
    esabi_value_set_string(&result, value);
    return result;
}

static std::string take_string(esabi_value &value) {
    const char *text = esabi_value_get_string(&value);
    std::string result = text ? text : "";
    if (text) ESFreeMem(value.payload.string_value);
    esabi_value_set_undefined(&value);
    return result;
}

static std::string ascii_hex(const std::string &text) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.resize(text.size() * 2u);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        out[i * 2u] = digits[(byte >> 4u) & 0x0fu];
        out[i * 2u + 1u] = digits[byte & 0x0fu];
    }
    return out;
}

static void expect_query_failure(
    esabi_i32 handle,
    const std::string &sql_hex,
    const std::string &parameter_wire,
    esabi_i32 max_rows,
    esabi_value *result) {
    esabi_value args[4] = {
        make_i32(handle),
        make_string(const_cast<char *>(sql_hex.c_str())),
        make_string(const_cast<char *>(parameter_wire.c_str())),
        make_i32(max_rows)
    };
    CHECK(querySql(args, 4, result) == ESABI_OK);
    CHECK(result->type == ESABI_TYPE_UNDEFINED);
}

static bool file_exists(const char *path) {
    std::FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

static void cleanup_db(const char *path) {
    std::remove(path);
    std::string wal = std::string(path) + "-wal";
    std::string shm = std::string(path) + "-shm";
    std::remove(wal.c_str());
    std::remove(shm.c_str());
}

int main() {
    const char *path = "esdb-externalobject-smoke.sqlite";
    std::remove(path);
    std::remove("esdb-externalobject-smoke.sqlite-wal");
    std::remove("esdb-externalobject-smoke.sqlite-shm");

    char *signatures = ESInitialize(nullptr, 0);
    CHECK(signatures != nullptr);
    CHECK(std::strstr(signatures, "stage_s") != nullptr);
    CHECK(std::strstr(signatures, "stageHex_s") != nullptr);
    CHECK(std::strstr(signatures, "openStaged_f") != nullptr);
    CHECK(std::strstr(signatures, "transactionBegin_dd") != nullptr);
    CHECK(std::strstr(signatures, "querySql_dssd") != nullptr);
    CHECK(std::strstr(signatures, "objectStorePutText_dssds") != nullptr);
    CHECK(std::strstr(signatures, "objectStoreChanges_dssd") != nullptr);
    CHECK(std::strstr(signatures, "objectStoreScan_dssd") != nullptr);
    CHECK(std::strstr(signatures, "storePutText_ssds") != nullptr);
    CHECK(std::strstr(signatures, "storePatch_ss") != nullptr);
    CHECK(std::strstr(signatures, "storeScan_ssd") != nullptr);
    CHECK(std::strstr(signatures, "storeGet_ss") != nullptr);
    CHECK(std::strstr(signatures, "storeChanges_ssd") != nullptr);
    CHECK(ESGetVersion() == 1);

    esabi_value dummy = make_double(0.0);
    esabi_value result{};
    CHECK(ping(&dummy, 1, &result) == ESABI_OK);
    esabi_i32 ping_value = 0;
    CHECK(esabi_value_get_i32(&result, &ping_value) != 0);
    CHECK(ping_value == 42);

    /* stage/openStaged state is thread-local, not process-global. */
    const char *tls_path_a = "esdb-externalobject-tls-a.sqlite";
    const char *tls_path_b = "esdb-externalobject-tls-b.sqlite";
    cleanup_db(tls_path_a);
    cleanup_db(tls_path_b);
    std::atomic<int> staged_threads{0};
    std::atomic<int> tls_failures{0};
    auto tls_worker = [&](const char *thread_path) {
        std::string mutable_thread_path(thread_path);
        esabi_value local_path = make_string(&mutable_thread_path[0]);
        esabi_value local_result{};
        if (stage(&local_path, 1, &local_result) != ESABI_OK) {
            tls_failures.fetch_add(1, std::memory_order_relaxed);
            staged_threads.fetch_add(1, std::memory_order_release);
            return;
        }
        int staged_ok = 0;
        if (!esabi_value_get_bool(&local_result, &staged_ok) || !staged_ok) {
            tls_failures.fetch_add(1, std::memory_order_relaxed);
        }
        staged_threads.fetch_add(1, std::memory_order_release);
        while (staged_threads.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }

        esabi_value local_dummy = make_double(0.0);
        if (openStaged(&local_dummy, 1, &local_result) != ESABI_OK) {
            tls_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        esabi_i32 local_handle = 0;
        if (!esabi_value_get_i32(&local_result, &local_handle) || local_handle <= 0) {
            tls_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        esabi_value local_handle_arg = make_i32(local_handle);
        if (close(&local_handle_arg, 1, &local_result) != ESABI_OK) {
            tls_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        int local_closed = 0;
        if (!esabi_value_get_bool(&local_result, &local_closed) || !local_closed) {
            tls_failures.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread tls_a(tls_worker, tls_path_a);
    std::thread tls_b(tls_worker, tls_path_b);
    tls_a.join();
    tls_b.join();
    CHECK(tls_failures.load(std::memory_order_relaxed) == 0);
    CHECK(file_exists(tls_path_a));
    CHECK(file_exists(tls_path_b));
    cleanup_db(tls_path_a);
    cleanup_db(tls_path_b);

    esabi_value version_result{};
    CHECK(version(&dummy, 1, &version_result) == ESABI_OK);
    CHECK(take_string(version_result) == "0.2.0");

    esabi_value sqlite_result{};
    CHECK(sqliteVersion(&dummy, 1, &sqlite_result) == ESABI_OK);
    CHECK(take_string(sqlite_result) == "3.53.4");

    /*
     * Native C/C++ and ExternalObject must resolve one ESDBCore module so the
     * Zustand-like process-memory Store is genuinely shared across surfaces.
     */
    const char *shared_store_name = "esdb.cross-surface";
    esdb_error native_error{};
    const esdb_status stale_destroy =
        esdb_store_destroy(shared_store_name, &native_error);
    CHECK(stale_destroy == ESDB_OK || stale_destroy == ESDB_ERR_NOT_FOUND);

    esdb_store *native_store = nullptr;
    CHECK(esdb_store_open(
        shared_store_name, &native_store, &native_error) == ESDB_OK);
    esdb_value *native_value = nullptr;
    const char native_text[] = "native";
    CHECK(esdb_value_create_text(
        ESDB_VALUE_UTF8,
        native_text,
        sizeof(native_text) - 1u,
        &native_value,
        &native_error) == ESDB_OK);
    CHECK(esdb_store_put(
        native_store, "owner", native_value, nullptr, &native_error) == ESDB_OK);
    esdb_value_destroy(native_value);
    native_value = nullptr;

    char shared_name_hex[] = "657364622E63726F73732D73757266616365";
    char owner_key_hex[] = "6F776E6572";
    esabi_value memory_get_args[2] = {
        make_string(shared_name_hex),
        make_string(owner_key_hex)
    };
    CHECK(storeGet(memory_get_args, 2, &result) == ESABI_OK);
    CHECK(take_string(result) == "V1:5:6E6174697665");

    char adapter_key_hex[] = "61646170746572";
    char adapter_text_hex[] = "6A7378"; /* jsx */
    esabi_value memory_put_args[4] = {
        make_string(shared_name_hex),
        make_string(adapter_key_hex),
        make_i32(ESDB_VALUE_UTF8),
        make_string(adapter_text_hex)
    };
    CHECK(storePutText(memory_put_args, 4, &result) == ESABI_OK);
    CHECK(!take_string(result).empty());

    /* JSX-style multi-key patch is atomic in the shared native Store. */
    char patch_wire[] =
        "P1:2\n"
        "616C706861:2:N:123\n"
        "62657461:5:T:74657874";
    esabi_value memory_patch_args[2] = {
        make_string(shared_name_hex),
        make_string(patch_wire)
    };
    CHECK(storePatch(memory_patch_args, 2, &result) == ESABI_OK);
    CHECK(!take_string(result).empty());

    CHECK(esdb_store_get(
        native_store, "adapter", &native_value, &native_error) == ESDB_OK);
    const void *native_bytes = nullptr;
    std::uint64_t native_size = 0u;
    CHECK(esdb_value_get_data(
        native_value, &native_bytes, &native_size) == ESDB_OK);
    CHECK(native_size == 3u);
    CHECK(std::memcmp(native_bytes, "jsx", 3u) == 0);
    esdb_value_destroy(native_value);
    native_value = nullptr;

    CHECK(esdb_store_get(
        native_store, "alpha", &native_value, &native_error) == ESDB_OK);
    std::int32_t patched_number = 0;
    CHECK(esdb_value_get_int32(native_value, &patched_number) == ESDB_OK);
    CHECK(patched_number == 123);
    esdb_value_destroy(native_value);
    native_value = nullptr;

    CHECK(esdb_store_get(
        native_store, "beta", &native_value, &native_error) == ESDB_OK);
    native_bytes = nullptr;
    native_size = 0u;
    CHECK(esdb_value_get_data(
        native_value, &native_bytes, &native_size) == ESDB_OK);
    CHECK(native_size == 4u);
    CHECK(std::memcmp(native_bytes, "text", 4u) == 0);
    esdb_value_destroy(native_value);
    native_value = nullptr;

    char empty_after_hex[] = "";
    esabi_value memory_scan_args[3] = {
        make_string(shared_name_hex),
        make_string(empty_after_hex),
        make_i32(16)
    };
    CHECK(storeScan(memory_scan_args, 3, &result) == ESABI_OK);
    const std::string memory_scan_wire = take_string(result);
    CHECK(memory_scan_wire.find("V1:4\n") == 0);
    CHECK(memory_scan_wire.find(":616C706861:2:123") != std::string::npos);
    CHECK(memory_scan_wire.find(":62657461:5:74657874") != std::string::npos);

    esdb_store_close(native_store);
    native_store = nullptr;
    esabi_value destroy_store_arg = make_string(shared_name_hex);
    CHECK(storeDestroy(&destroy_store_arg, 1, &result) == ESABI_OK);
    int store_destroyed = 0;
    CHECK(esabi_value_get_bool(&result, &store_destroyed) != 0);
    CHECK(store_destroyed == 1);

    char mutable_path[] = "esdb-externalobject-smoke.sqlite";
    esabi_value path_arg = make_string(mutable_path);
    CHECK(stage(&path_arg, 1, &result) == ESABI_OK);
    int staged = 0;
    CHECK(esabi_value_get_bool(&result, &staged) != 0);
    CHECK(staged == 1);

    /*
     * Keep a direct native Runtime connection open while ExternalObject opens a
     * second connection to the same WAL database. This is the deployment model
     * for native Adobe plug-ins and JSX sharing persistent state safely.
     */
    esdb_open_options native_options{};
    esdb_open_options_init(&native_options);
    native_options.journal_mode = ESDB_JOURNAL_WAL;
    esdb_database *native_database = nullptr;
    esdb_error persistent_error{};
    CHECK(esdb_open(
        path, &native_options, &native_database, &persistent_error) == ESDB_OK);
    CHECK(native_database != nullptr);

    CHECK(openStaged(&dummy, 1, &result) == ESABI_OK);
    esabi_i32 handle = 0;
    CHECK(esabi_value_get_i32(&result, &handle) != 0);
    CHECK(handle > 0);

    CHECK(handleCount(&dummy, 1, &result) == ESABI_OK);
    esabi_i32 count = 0;
    CHECK(esabi_value_get_i32(&result, &count) != 0);
    CHECK(count == 1);

    esabi_value handle_arg = make_i32(handle);
    CHECK(health(&handle_arg, 1, &result) == ESABI_OK);
    const std::string health_text = take_string(result);
    CHECK(health_text.find("\"ok\":true") != std::string::npos);
    CHECK(health_text.find("\"pageSize\":") != std::string::npos);

    CHECK(dataVersion(&handle_arg, 1, &result) == ESABI_OK);
    double data_version = -1.0;
    CHECK(esabi_value_get_double(&result, &data_version) != 0);
    CHECK(data_version >= 0.0);


    /* Typed SQL escape hatch: SQL and values are separate ABI arguments. */
    CHECK(esdb_exec(
        native_database,
        "CREATE TABLE raw_probe("
        "i INTEGER PRIMARY KEY,t TEXT NOT NULL,b BLOB NOT NULL,r REAL NOT NULL,n TEXT);",
        &persistent_error) == ESDB_OK);

    std::string raw_insert_sql_hex = ascii_hex(
        "INSERT INTO raw_probe(i,t,b,r,n) VALUES(?,?,?,?,?)");
    std::string raw_insert_params =
        "P1:5\n"
        "V1:3:9007199254740993\n"
        "V1:5:68656C6C6F\n"
        "V1:6:00FF\n"
        "V1:4:3.25\n"
        "V1:0:";
    esabi_value raw_insert_args[4] = {
        make_i32(handle),
        make_string(&raw_insert_sql_hex[0]),
        make_string(&raw_insert_params[0]),
        make_i32(0)
    };
    CHECK(querySql(raw_insert_args, 4, &result) == ESABI_OK);
    const std::string raw_insert_wire = take_string(result);
    CHECK(raw_insert_wire.find("Q1:0:0:0:1:0\nN:\n") == 0u);

    std::string raw_select_sql_hex = ascii_hex(
        "SELECT i,t,b,r,n FROM raw_probe WHERE i=?");
    std::string raw_select_params = "P1:1\nV1:3:9007199254740993";
    esabi_value raw_select_args[4] = {
        make_i32(handle),
        make_string(&raw_select_sql_hex[0]),
        make_string(&raw_select_params[0]),
        make_i32(100)
    };
    CHECK(querySql(raw_select_args, 4, &result) == ESABI_OK);
    const std::string raw_select_wire = take_string(result);
    CHECK(raw_select_wire.find("Q1:5:1:1:0:0\n") == 0u);
    CHECK(raw_select_wire.find("N:69|74|62|72|6E\n") != std::string::npos);
    CHECK(raw_select_wire.find(
        "R:3:9007199254740993|5:68656C6C6F|6:00FF|4:3.25|0:\n") !=
        std::string::npos);

    std::string raw_empty_result_sql_hex = ascii_hex(
        "SELECT 1 AS empty_result WHERE 0");
    std::string raw_empty_result_params = "P1:0";
    esabi_value raw_empty_result_args[4] = {
        make_i32(handle),
        make_string(&raw_empty_result_sql_hex[0]),
        make_string(&raw_empty_result_params[0]),
        make_i32(10)
    };
    CHECK(querySql(raw_empty_result_args, 4, &result) == ESABI_OK);
    const std::string raw_empty_result_wire = take_string(result);
    CHECK(raw_empty_result_wire.find(
        "Q1:1:0:0:0:0\nN:656D7074795F726573756C74\n") == 0u);

    std::string raw_truncate_sql_hex = ascii_hex(
        "SELECT 1 AS x UNION ALL SELECT 2 UNION ALL SELECT 3");
    std::string raw_no_params = "P1:0";
    esabi_value raw_truncate_args[4] = {
        make_i32(handle),
        make_string(&raw_truncate_sql_hex[0]),
        make_string(&raw_no_params[0]),
        make_i32(1)
    };
    CHECK(querySql(raw_truncate_args, 4, &result) == ESABI_OK);
    const std::string raw_truncate_wire = take_string(result);
    CHECK(raw_truncate_wire.find("Q1:1:1:3:0:1\n") == 0u);
    CHECK(raw_truncate_wire.find("R:3:1\n") != std::string::npos);

    std::string raw_bad_params = "P1:0";
    esabi_value raw_bad_args[4] = {
        make_i32(handle),
        make_string(&raw_select_sql_hex[0]),
        make_string(&raw_bad_params[0]),
        make_i32(10)
    };
    CHECK(querySql(raw_bad_args, 4, &result) == ESABI_OK);
    CHECK(result.type == ESABI_TYPE_UNDEFINED);

    std::string distinct_sql_hex = ascii_hex("SELECT ?, ?, ?, ?, ?");
    std::string distinct_params =
        "P1:5\n"
        "V1:0:\n"
        "V1:5:\n"
        "V1:2:0\n"
        "V1:6:\n"
        "V1:1:0";
    esabi_value distinct_args[4] = {
        make_i32(handle),
        make_string(&distinct_sql_hex[0]),
        make_string(&distinct_params[0]),
        make_i32(10)
    };
    CHECK(querySql(distinct_args, 4, &result) == ESABI_OK);
    const std::string distinct_wire = take_string(result);
    CHECK(distinct_wire.find("Q1:5:1:1:0:0\n") == 0u);
    CHECK(distinct_wire.find("R:0:|5:|3:0|6:|3:0\n") != std::string::npos);

    std::string min_max_sql_hex = ascii_hex("SELECT ?, ?");
    std::string min_max_params =
        "P1:2\n"
        "V1:3:-9223372036854775808\n"
        "V1:3:9223372036854775807";
    esabi_value min_max_args[4] = {
        make_i32(handle),
        make_string(&min_max_sql_hex[0]),
        make_string(&min_max_params[0]),
        make_i32(10)
    };
    CHECK(querySql(min_max_args, 4, &result) == ESABI_OK);
    const std::string min_max_wire = take_string(result);
    CHECK(min_max_wire.find(
        "R:3:-9223372036854775808|3:9223372036854775807\n") != std::string::npos);

    std::string exact_text_sql_hex = ascii_hex("SELECT ?");
    std::string exact_text_params = "P1:1\nV1:5:610062F09F9880";
    esabi_value exact_text_args[4] = {
        make_i32(handle),
        make_string(&exact_text_sql_hex[0]),
        make_string(&exact_text_params[0]),
        make_i32(10)
    };
    CHECK(querySql(exact_text_args, 4, &result) == ESABI_OK);
    const std::string exact_text_wire = take_string(result);
    CHECK(exact_text_wire.find("R:5:610062F09F9880\n") != std::string::npos);

    std::string double_sql_hex = ascii_hex("SELECT ?, ?, ?, ?");
    std::string double_params =
        "P1:4\n"
        "V1:4:3.25\n"
        "V1:4:Infinity\n"
        "V1:4:-Infinity\n"
        "V1:4:-0";
    esabi_value double_args[4] = {
        make_i32(handle),
        make_string(&double_sql_hex[0]),
        make_string(&double_params[0]),
        make_i32(10)
    };
    CHECK(querySql(double_args, 4, &result) == ESABI_OK);
    const std::string double_wire = take_string(result);
    CHECK(double_wire.find("R:4:3.25|4:Infinity|4:-Infinity|4:-0\n") != std::string::npos);

    expect_query_failure(
        handle, ascii_hex("SELECT ?"), "P1:1\nV1:4:NaN", 10, &result);

    const std::string injection = "x'); DROP TABLE raw_probe; --";
    std::string injection_sql_hex = ascii_hex("SELECT ?");
    std::string injection_params = "P1:1\nV1:5:" + ascii_hex(injection);
    esabi_value injection_args[4] = {
        make_i32(handle),
        make_string(&injection_sql_hex[0]),
        make_string(&injection_params[0]),
        make_i32(10)
    };
    CHECK(querySql(injection_args, 4, &result) == ESABI_OK);
    const std::string injection_wire = take_string(result);
    CHECK(injection_wire.find("R:5:" + ascii_hex(injection) + "\n") != std::string::npos);

    std::string table_count_sql_hex = ascii_hex(
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='raw_probe'");
    std::string table_count_params = "P1:0";
    esabi_value table_count_args[4] = {
        make_i32(handle),
        make_string(&table_count_sql_hex[0]),
        make_string(&table_count_params[0]),
        make_i32(10)
    };
    CHECK(querySql(table_count_args, 4, &result) == ESABI_OK);
    const std::string table_count_wire = take_string(result);
    CHECK(table_count_wire.find("R:3:1\n") != std::string::npos);

    std::string max_rows_sql_hex = ascii_hex("SELECT 1 AS x");
    std::string max_rows_params = "P1:0";
    esabi_value max_zero_args[4] = {
        make_i32(handle),
        make_string(&max_rows_sql_hex[0]),
        make_string(&max_rows_params[0]),
        make_i32(0)
    };
    CHECK(querySql(max_zero_args, 4, &result) == ESABI_OK);
    CHECK(take_string(result).find("Q1:1:0:1:0:1\n") == 0u);
    esabi_value max_bound_args[4] = {
        make_i32(handle),
        make_string(&max_rows_sql_hex[0]),
        make_string(&max_rows_params[0]),
        make_i32(10000)
    };
    CHECK(querySql(max_bound_args, 4, &result) == ESABI_OK);
    CHECK(take_string(result).find("Q1:1:1:1:0:0\n") == 0u);

    expect_query_failure(handle, "0G", "P1:0", 10, &result);
    expect_query_failure(handle, "00", "P1:0", 10, &result);
    expect_query_failure(handle, ascii_hex("NOT SQL"), "P1:0", 10, &result);
    expect_query_failure(
        handle, ascii_hex("SELECT 1; SELECT 2"), "P1:0", 10, &result);
    expect_query_failure(
        handle, ascii_hex("SELECT ?"),
        "P1:2\nV1:3:1\nV1:3:2", 10, &result);
    expect_query_failure(
        handle, ascii_hex("SELECT ?"), "P1:0\nV1:3:1", 10, &result);
    expect_query_failure(
        handle, ascii_hex("SELECT ?"), "P1:1\nV1:5:C3", 10, &result);
    expect_query_failure(
        handle, ascii_hex("SELECT ?"), "P1:1\nV1:6:0", 10, &result);
    expect_query_failure(
        handle, ascii_hex("SELECT 1"), "P1:1025", 10, &result);
    expect_query_failure(
        handle, ascii_hex("SELECT 1"), "P1:0", -1, &result);
    expect_query_failure(
        handle, ascii_hex("SELECT 1"), "P1:0", 10001, &result);
    std::string oversized_sql(1024u * 1024u + 1u, ' ');
    expect_query_failure(
        handle, ascii_hex(oversized_sql), "P1:0", 10, &result);
    std::string oversized_packet(8u * 1024u * 1024u + 1u, 'x');
    expect_query_failure(
        handle, ascii_hex("SELECT 1"), oversized_packet, 10, &result);

    std::string invalid_utf8_sql = "SELECT '";
    invalid_utf8_sql.push_back(static_cast<char>(0xc3));
    invalid_utf8_sql += "'";
    expect_query_failure(
        handle, ascii_hex(invalid_utf8_sql), "P1:0", 10, &result);

    std::string cap_sql_hex = ascii_hex("SELECT zeroblob(8388608)");
    std::string cap_params = "P1:0";
    esabi_value cap_args[4] = {
        make_i32(handle),
        make_string(&cap_sql_hex[0]),
        make_string(&cap_params[0]),
        make_i32(1)
    };
    CHECK(querySql(cap_args, 4, &result) == ESABI_OK);
    const std::string cap_wire = take_string(result);
    CHECK(cap_wire.find("Q1:1:0:1:0:1\n") == 0u);
    CHECK(cap_wire.size() <= 16u * 1024u * 1024u);

    std::string recovery_sql_hex = ascii_hex("SELECT 1 AS recovered");
    std::string recovery_params = "P1:0";
    esabi_value recovery_args[4] = {
        make_i32(handle),
        make_string(&recovery_sql_hex[0]),
        make_string(&recovery_params[0]),
        make_i32(10)
    };
    CHECK(querySql(recovery_args, 4, &result) == ESABI_OK);
    CHECK(take_string(result).find("Q1:1:1:1:0:0\n") == 0u);


    /*
     * Persistent cross-surface visibility: native writes -> JSX reads, then
     * JSX writes -> native reads, while both SQLite connections stay open.
     */
    const char *shared_object_store = "shared-db";
    CHECK(esdb_object_store_ensure(
        native_database, shared_object_store, &persistent_error) == ESDB_OK);
    esdb_value *persistent_value = nullptr;
    CHECK(esdb_value_create_text(
        ESDB_VALUE_UTF8, "cpp", 3u,
        &persistent_value, &persistent_error) == ESDB_OK);
    CHECK(esdb_object_store_put(
        native_database,
        shared_object_store,
        "native",
        persistent_value,
        nullptr,
        &persistent_error) == ESDB_OK);
    esdb_value_destroy(persistent_value);
    persistent_value = nullptr;

    char shared_object_hex[] = "7368617265642D6462"; /* shared-db */
    char native_key_hex[] = "6E6174697665";         /* native */
    esabi_value persistent_get_args[3] = {
        make_i32(handle),
        make_string(shared_object_hex),
        make_string(native_key_hex)
    };
    CHECK(objectStoreGet(persistent_get_args, 3, &result) == ESABI_OK);
    CHECK(take_string(result) == "V1:5:637070");

    char jsx_key_hex[] = "6A7378"; /* jsx */
    char eso_text_hex[] = "65736F"; /* eso */
    esabi_value persistent_put_args[5] = {
        make_i32(handle),
        make_string(shared_object_hex),
        make_string(jsx_key_hex),
        make_i32(ESDB_VALUE_UTF8),
        make_string(eso_text_hex)
    };
    CHECK(objectStorePutText(persistent_put_args, 5, &result) == ESABI_OK);
    CHECK(!take_string(result).empty());

    CHECK(esdb_object_store_get(
        native_database,
        shared_object_store,
        "jsx",
        &persistent_value,
        &persistent_error) == ESDB_OK);
    const void *persistent_bytes = nullptr;
    std::uint64_t persistent_size = 0u;
    CHECK(esdb_value_get_data(
        persistent_value,
        &persistent_bytes,
        &persistent_size) == ESDB_OK);
    CHECK(persistent_size == 3u);
    CHECK(std::memcmp(persistent_bytes, "eso", 3u) == 0);
    esdb_value_destroy(persistent_value);
    persistent_value = nullptr;

    /* Typed durable ObjectStore surface uses byte-exact ASCII-hex strings. */
    char store_hex[] = "73657474696E6773"; /* settings */
    char theme_hex[] = "7468656D65";       /* theme */
    char dark_hex[] = "6461726B";           /* dark */
    esabi_value ensure_args[2] = {
        make_i32(handle),
        make_string(store_hex)
    };
    CHECK(objectStoreEnsure(ensure_args, 2, &result) == ESABI_OK);
    int ensured = 0;
    CHECK(esabi_value_get_bool(&result, &ensured) != 0);
    CHECK(ensured == 1);

    esabi_value put_text_args[5] = {
        make_i32(handle),
        make_string(store_hex),
        make_string(theme_hex),
        make_i32(ESDB_VALUE_UTF8),
        make_string(dark_hex)
    };
    CHECK(objectStorePutText(put_text_args, 5, &result) == ESABI_OK);
    const std::string first_revision = take_string(result);
    CHECK(!first_revision.empty());
    CHECK(first_revision != "0");

    esabi_value get_args[3] = {
        make_i32(handle),
        make_string(store_hex),
        make_string(theme_hex)
    };
    CHECK(objectStoreGet(get_args, 3, &result) == ESABI_OK);
    CHECK(take_string(result) == "V1:5:6461726B");

    /* Exact INT64 does not round through ExternalObject's double lane. */
    char big_key_hex[] = "626967"; /* big */
    char exact_i64[] = "9007199254740993";
    esabi_value put_i64_args[5] = {
        make_i32(handle),
        make_string(store_hex),
        make_string(big_key_hex),
        make_i32(ESDB_VALUE_INT64),
        make_string(exact_i64)
    };
    CHECK(objectStorePutText(put_i64_args, 5, &result) == ESABI_OK);
    const std::string second_revision = take_string(result);
    CHECK(second_revision != first_revision);

    esabi_value get_i64_args[3] = {
        make_i32(handle),
        make_string(store_hex),
        make_string(big_key_hex)
    };
    CHECK(objectStoreGet(get_i64_args, 3, &result) == ESABI_OK);
    CHECK(take_string(result) == "V1:3:9007199254740993");

    /* Adapter-owned transaction handle: rollback and commit semantics. */
    esabi_value begin_args[2] = {
        make_i32(handle),
        make_i32(ESDB_TRANSACTION_IMMEDIATE)
    };
    CHECK(transactionBegin(begin_args, 2, &result) == ESABI_OK);
    int began = 0;
    CHECK(esabi_value_get_bool(&result, &began) != 0);
    CHECK(began == 1);
    CHECK(transactionActive(&handle_arg, 1, &result) == ESABI_OK);
    int transaction_active = 0;
    CHECK(esabi_value_get_bool(&result, &transaction_active) != 0);
    CHECK(transaction_active == 1);

    char rollback_key_hex[] = "726F6C6C6261636B"; /* rollback */
    esabi_value rollback_put_args[5] = {
        make_i32(handle),
        make_string(store_hex),
        make_string(rollback_key_hex),
        make_i32(ESDB_VALUE_INT32),
        make_double(42.0)
    };
    CHECK(objectStorePutNumber(rollback_put_args, 5, &result) == ESABI_OK);
    (void)take_string(result);
    CHECK(transactionRollback(&handle_arg, 1, &result) == ESABI_OK);
    int rolled_back = 0;
    CHECK(esabi_value_get_bool(&result, &rolled_back) != 0);
    CHECK(rolled_back == 1);

    esabi_value rollback_get_args[3] = {
        make_i32(handle),
        make_string(store_hex),
        make_string(rollback_key_hex)
    };
    CHECK(objectStoreGet(rollback_get_args, 3, &result) == ESABI_OK);
    CHECK(take_string(result) == "V1:-1:");

    CHECK(transactionBegin(begin_args, 2, &result) == ESABI_OK);
    CHECK(esabi_value_get_bool(&result, &began) != 0);
    CHECK(began == 1);
    char committed_key_hex[] = "636F6D6D6974746564"; /* committed */
    esabi_value commit_put_args[5] = {
        make_i32(handle),
        make_string(store_hex),
        make_string(committed_key_hex),
        make_i32(ESDB_VALUE_BOOL),
        make_double(1.0)
    };
    CHECK(objectStorePutNumber(commit_put_args, 5, &result) == ESABI_OK);
    (void)take_string(result);
    CHECK(transactionCommit(&handle_arg, 1, &result) == ESABI_OK);
    int committed = 0;
    CHECK(esabi_value_get_bool(&result, &committed) != 0);
    CHECK(committed == 1);

    esabi_value count_args[2] = {
        make_i32(handle),
        make_string(store_hex)
    };
    CHECK(objectStoreCount(count_args, 2, &result) == ESABI_OK);
    CHECK(take_string(result) == "3");

    char durable_empty_after_hex[] = "";
    esabi_value object_scan_args[4] = {
        make_i32(handle),
        make_string(store_hex),
        make_string(durable_empty_after_hex),
        make_i32(16)
    };
    CHECK(objectStoreScan(object_scan_args, 4, &result) == ESABI_OK);
    const std::string object_scan_wire = take_string(result);
    CHECK(object_scan_wire.find("V1:3\n") == 0);
    CHECK(object_scan_wire.find(":626967:3:9007199254740993") != std::string::npos);
    CHECK(object_scan_wire.find(":7468656D65:5:6461726B") != std::string::npos);

    CHECK(objectStoreRevision(&handle_arg, 1, &result) == ESABI_OK);
    const std::string current_revision = take_string(result);
    CHECK(!current_revision.empty());

    char all_stores[] = "";
    char revision_zero[] = "0";
    esabi_value changes_args[4] = {
        make_i32(handle),
        make_string(all_stores),
        make_string(revision_zero),
        make_i32(100)
    };
    CHECK(objectStoreChanges(changes_args, 4, &result) == ESABI_OK);
    const std::string change_wire = take_string(result);
    CHECK(change_wire.find("V1:") == 0);
    CHECK(change_wire.find(":1:5:73657474696E6773:7468656D65") != std::string::npos);
    CHECK(change_wire.find(":1:3:73657474696E6773:626967") != std::string::npos);

    esabi_value prune_args[2] = {
        make_i32(handle),
        make_string(const_cast<char *>(current_revision.c_str()))
    };
    CHECK(objectStorePrune(prune_args, 2, &result) == ESABI_OK);
    const std::string pruned = take_string(result);
    CHECK(!pruned.empty());

    CHECK(close(&handle_arg, 1, &result) == ESABI_OK);
    int closed = 0;
    CHECK(esabi_value_get_bool(&result, &closed) != 0);
    CHECK(closed == 1);

    /* The old token must be rejected after generation advances. */
    CHECK(health(&handle_arg, 1, &result) == ESABI_OK);
    CHECK(result.type == ESABI_TYPE_UNDEFINED);

    CHECK(lastError(&dummy, 1, &result) == ESABI_OK);
    const std::string error_text = take_string(result);
    CHECK(error_text.find("\"ok\":false") != std::string::npos);
    CHECK(error_text.find("\"adapterCode\":5") != std::string::npos);

    CHECK(openStaged(&dummy, 1, &result) == ESABI_OK);
    esabi_i32 second_handle = 0;
    CHECK(esabi_value_get_i32(&result, &second_handle) != 0);
    CHECK(second_handle > 0);
    CHECK(second_handle != handle);

    ESTerminate();
    esdb_close(native_database);
    native_database = nullptr;

    CHECK(handleCount(&dummy, 1, &result) == ESABI_OK);
    CHECK(esabi_value_get_i32(&result, &count) != 0);
    CHECK(count == 0);

    std::remove(path);
    std::remove("esdb-externalobject-smoke.sqlite-wal");
    std::remove("esdb-externalobject-smoke.sqlite-shm");

    if (failures) {
        std::fprintf(stderr, "%d ExternalObject smoke checks failed\n", failures);
        return 1;
    }
    std::puts("ESDB ExternalObject ABI smoke: PASS");
    return 0;
}
