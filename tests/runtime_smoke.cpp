#include <esdb/esdb.h>
#include <esdb/esdb_object_store.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
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

static int capture_change(const esdb_object_change *change, void *user) {
    auto *capture = static_cast<ChangeCapture *>(user);
    CHECK(change != nullptr);
    CHECK(change->store_name != nullptr);
    CHECK(change->key != nullptr);
    ++capture->count;
    capture->last = change->revision;
    return 1;
}


struct QueryCapture {
    uint32_t callbacks = 0u;
    bool exact_int64 = false;
    bool text = false;
    bool blob = false;
    bool real = false;
    bool null_value = false;
    bool bool_value = false;
    bool int32_value = false;
};

static int capture_query_row(
    const char *const *names,
    const esdb_value *const *values,
    uint32_t count,
    void *user) {
    auto *capture = static_cast<QueryCapture *>(user);
    CHECK(capture != nullptr);
    CHECK(names != nullptr);
    CHECK(values != nullptr);
    CHECK(count == 7u);
    if (!capture || !names || !values || count != 7u) return 1;

    ++capture->callbacks;
    CHECK(std::strcmp(names[0], "i") == 0);
    CHECK(std::strcmp(names[1], "t") == 0);
    CHECK(std::strcmp(names[2], "b") == 0);
    CHECK(std::strcmp(names[3], "r") == 0);
    CHECK(std::strcmp(names[4], "n") == 0);
    CHECK(std::strcmp(names[5], "bo") == 0);
    CHECK(std::strcmp(names[6], "i32v") == 0);

    int64_t i64 = 0;
    CHECK(esdb_value_type_of(values[0]) == ESDB_VALUE_INT64);
    CHECK(esdb_value_get_int64(values[0], &i64) == ESDB_OK);
    capture->exact_int64 = i64 == INT64_C(9007199254740993);

    const void *data = nullptr;
    uint64_t size = 0u;
    CHECK(esdb_value_type_of(values[1]) == ESDB_VALUE_UTF8);
    CHECK(esdb_value_get_data(values[1], &data, &size) == ESDB_OK);
    capture->text = size == 5u && data && std::memcmp(data, "hello", 5u) == 0;

    data = nullptr;
    size = 0u;
    CHECK(esdb_value_type_of(values[2]) == ESDB_VALUE_BYTES);
    CHECK(esdb_value_get_data(values[2], &data, &size) == ESDB_OK);
    const unsigned char expected_blob[] = {0u, 255u, 1u};
    capture->blob =
        size == sizeof(expected_blob) && data &&
        std::memcmp(data, expected_blob, sizeof(expected_blob)) == 0;

    double real = 0.0;
    CHECK(esdb_value_type_of(values[3]) == ESDB_VALUE_DOUBLE);
    CHECK(esdb_value_get_double(values[3], &real) == ESDB_OK);
    capture->real = real == 3.25;

    capture->null_value = esdb_value_type_of(values[4]) == ESDB_VALUE_NULL;

    int64_t bool_as_int = 0;
    CHECK(esdb_value_type_of(values[5]) == ESDB_VALUE_INT64);
    CHECK(esdb_value_get_int64(values[5], &bool_as_int) == ESDB_OK);
    capture->bool_value = bool_as_int == 1;

    int64_t int32_as_int = 0;
    CHECK(esdb_value_type_of(values[6]) == ESDB_VALUE_INT64);
    CHECK(esdb_value_get_int64(values[6], &int32_as_int) == ESDB_OK);
    capture->int32_value = int32_as_int == -123;
    return 0;
}

struct Int64Capture {
    bool seen = false;
    int64_t value = 0;
};

static int capture_int64(
    const char *const *,
    const esdb_value *const *values,
    uint32_t count,
    void *user) {
    auto *capture = static_cast<Int64Capture *>(user);
    CHECK(capture != nullptr);
    CHECK(values != nullptr);
    CHECK(count == 1u);
    if (!capture || !values || count != 1u) return 1;
    CHECK(esdb_value_type_of(values[0]) == ESDB_VALUE_INT64);
    int64_t value = 0;
    CHECK(esdb_value_get_int64(values[0], &value) == ESDB_OK);
    capture->seen = true;
    capture->value = value;
    return 0;
}

struct EmptyBlobCapture {
    bool seen = false;
};

static int capture_empty_blob(
    const char *const *,
    const esdb_value *const *values,
    uint32_t count,
    void *user) {
    auto *capture = static_cast<EmptyBlobCapture *>(user);
    CHECK(capture != nullptr);
    CHECK(values != nullptr);
    CHECK(count == 1u);
    if (!capture || !values || count != 1u) return 1;
    const void *data = reinterpret_cast<const void *>(1);
    uint64_t size = 999u;
    CHECK(esdb_value_type_of(values[0]) == ESDB_VALUE_BYTES);
    CHECK(esdb_value_get_data(values[0], &data, &size) == ESDB_OK);
    capture->seen = size == 0u;
    return 1;
}

struct QueryValueCapture {
    uint32_t callbacks = 0u;
    uint32_t count = 0u;
    esdb_value_type types[8]{};
    uint64_t sizes[8]{};
    std::string bytes[8];
    int64_t integers[8]{};
    double reals[8]{};
};

static int capture_query_values(
    const char *const *,
    const esdb_value *const *values,
    uint32_t count,
    void *user) {
    auto *capture = static_cast<QueryValueCapture *>(user);
    CHECK(capture != nullptr);
    CHECK(values != nullptr);
    CHECK(count <= 8u);
    if (!capture || !values || count > 8u) return 1;

    ++capture->callbacks;
    capture->count = count;
    for (uint32_t index = 0u; index < count; ++index) {
        const esdb_value_type type = esdb_value_type_of(values[index]);
        capture->types[index] = type;
        if (type == ESDB_VALUE_UTF8 || type == ESDB_VALUE_BYTES ||
            type == ESDB_VALUE_ARRAY || type == ESDB_VALUE_OBJECT) {
            const void *data = nullptr;
            uint64_t size = 0u;
            CHECK(esdb_value_get_data(values[index], &data, &size) == ESDB_OK);
            capture->sizes[index] = size;
            if (data && size > 0u) {
                capture->bytes[index].assign(
                    static_cast<const char *>(data), static_cast<size_t>(size));
            }
        } else if (type == ESDB_VALUE_INT64) {
            CHECK(esdb_value_get_int64(values[index], &capture->integers[index]) == ESDB_OK);
        } else if (type == ESDB_VALUE_INT32) {
            std::int32_t value = 0;
            CHECK(esdb_value_get_int32(values[index], &value) == ESDB_OK);
            capture->integers[index] = value;
        } else if (type == ESDB_VALUE_BOOL) {
            int value = 0;
            CHECK(esdb_value_get_bool(values[index], &value) == ESDB_OK);
            capture->integers[index] = value;
        } else if (type == ESDB_VALUE_DOUBLE) {
            CHECK(esdb_value_get_double(values[index], &capture->reals[index]) == ESDB_OK);
        }
    }
    return 0;
}

static int throwing_query_row(
    const char *const *names,
    const esdb_value *const *values,
    uint32_t count,
    void *) {
    CHECK(names != nullptr);
    CHECK(values != nullptr);
    CHECK(count == 3u);
    if (!names || !values || count != 3u) {
        throw std::runtime_error("query callback received invalid row metadata");
    }
    CHECK(std::strcmp(names[0], "i") == 0);
    CHECK(std::strcmp(names[1], "t") == 0);
    CHECK(std::strcmp(names[2], "b") == 0);
    CHECK(esdb_value_type_of(values[0]) == ESDB_VALUE_INT64);
    CHECK(esdb_value_type_of(values[1]) == ESDB_VALUE_UTF8);
    CHECK(esdb_value_type_of(values[2]) == ESDB_VALUE_BYTES);
    throw std::runtime_error("intentional query callback fault");
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


    /* Typed prepared-query escape hatch: values never enter SQL text. */
    CHECK(esdb_exec(
        db,
        "CREATE TABLE query_probe("
        "i INTEGER NOT NULL,t TEXT NOT NULL,b BLOB NOT NULL,r REAL NOT NULL,"
        "n TEXT,bo INTEGER NOT NULL,i32v INTEGER NOT NULL);",
        &error) == ESDB_OK);

    esdb_value *query_params[7] = {};
    const unsigned char blob_bytes[] = {0u, 255u, 1u};
    CHECK(esdb_value_create_int64(INT64_C(9007199254740993), &query_params[0], &error) == ESDB_OK);
    CHECK(esdb_value_create_text(ESDB_VALUE_UTF8, "hello", 5u, &query_params[1], &error) == ESDB_OK);
    CHECK(esdb_value_create_bytes(blob_bytes, sizeof(blob_bytes), &query_params[2], &error) == ESDB_OK);
    CHECK(esdb_value_create_double(3.25, &query_params[3], &error) == ESDB_OK);
    CHECK(esdb_value_create_null(&query_params[4], &error) == ESDB_OK);
    CHECK(esdb_value_create_bool(1, &query_params[5], &error) == ESDB_OK);
    CHECK(esdb_value_create_int32(-123, &query_params[6], &error) == ESDB_OK);

    const esdb_value *query_param_refs[7] = {
        query_params[0], query_params[1], query_params[2], query_params[3],
        query_params[4], query_params[5], query_params[6]
    };
    uint64_t query_rows = 99u;
    uint64_t query_changes = 99u;
    CHECK(esdb_query(
        db,
        "INSERT INTO query_probe(i,t,b,r,n,bo,i32v) VALUES(?,?,?,?,?,?,?)",
        query_param_refs,
        7u,
        nullptr,
        nullptr,
        &query_rows,
        &query_changes,
        &error) == ESDB_OK);
    CHECK(query_rows == 0u);
    CHECK(query_changes == 1u);

    QueryCapture query_capture{};
    CHECK(esdb_query(
        db,
        "SELECT i,t,b,r,n,bo,i32v FROM query_probe WHERE i=?",
        query_param_refs,
        1u,
        capture_query_row,
        &query_capture,
        &query_rows,
        &query_changes,
        &error) == ESDB_OK);
    CHECK(query_rows == 1u);
    CHECK(query_changes == 0u);
    CHECK(query_capture.callbacks == 1u);
    CHECK(query_capture.exact_int64);
    CHECK(query_capture.text);
    CHECK(query_capture.blob);
    CHECK(query_capture.real);
    CHECK(query_capture.null_value);
    CHECK(query_capture.bool_value);
    CHECK(query_capture.int32_value);

    esdb_value *empty_blob = nullptr;
    CHECK(esdb_value_create_bytes(nullptr, 0u, &empty_blob, &error) == ESDB_OK);
    const esdb_value *empty_blob_params[] = {empty_blob};
    EmptyBlobCapture empty_capture{};
    CHECK(esdb_query(
        db,
        "SELECT ? AS empty_blob UNION ALL SELECT X'01'",
        empty_blob_params,
        1u,
        capture_empty_blob,
        &empty_capture,
        &query_rows,
        nullptr,
        &error) == ESDB_OK);
    CHECK(empty_capture.seen);
    CHECK(query_rows == 2u); /* callback stopped delivery; statement still completed */

    esdb_value *distinct_values[5] = {};
    CHECK(esdb_value_create_null(&distinct_values[0], &error) == ESDB_OK);
    CHECK(esdb_value_create_text(ESDB_VALUE_UTF8, nullptr, 0u, &distinct_values[1], &error) == ESDB_OK);
    CHECK(esdb_value_create_int32(0, &distinct_values[2], &error) == ESDB_OK);
    CHECK(esdb_value_create_bytes(nullptr, 0u, &distinct_values[3], &error) == ESDB_OK);
    CHECK(esdb_value_create_bool(0, &distinct_values[4], &error) == ESDB_OK);
    const esdb_value *distinct_refs[5] = {
        distinct_values[0], distinct_values[1], distinct_values[2],
        distinct_values[3], distinct_values[4]
    };
    QueryValueCapture distinct_capture{};
    CHECK(esdb_query(
        db, "SELECT ?, ?, ?, ?, ?", distinct_refs, 5u,
        capture_query_values, &distinct_capture, &query_rows, nullptr, &error) == ESDB_OK);
    CHECK(distinct_capture.callbacks == 1u);
    CHECK(distinct_capture.count == 5u);
    CHECK(distinct_capture.types[0] == ESDB_VALUE_NULL);
    CHECK(distinct_capture.types[1] == ESDB_VALUE_UTF8);
    CHECK(distinct_capture.sizes[1] == 0u);
    CHECK(distinct_capture.types[2] == ESDB_VALUE_INT64);
    CHECK(distinct_capture.integers[2] == 0);
    CHECK(distinct_capture.types[3] == ESDB_VALUE_BYTES);
    CHECK(distinct_capture.sizes[3] == 0u);
    CHECK(distinct_capture.types[4] == ESDB_VALUE_INT64);
    CHECK(distinct_capture.integers[4] == 0);

    esdb_value *min_value = nullptr;
    esdb_value *max_value = nullptr;
    CHECK(esdb_value_create_int64(
        std::numeric_limits<std::int64_t>::min(), &min_value, &error) == ESDB_OK);
    CHECK(esdb_value_create_int64(
        std::numeric_limits<std::int64_t>::max(), &max_value, &error) == ESDB_OK);
    const esdb_value *min_max_refs[2] = {min_value, max_value};
    QueryValueCapture min_max_capture{};
    CHECK(esdb_query(
        db, "SELECT ?, ?", min_max_refs, 2u,
        capture_query_values, &min_max_capture, &query_rows, nullptr, &error) == ESDB_OK);
    CHECK(min_max_capture.integers[0] == std::numeric_limits<std::int64_t>::min());
    CHECK(min_max_capture.integers[1] == std::numeric_limits<std::int64_t>::max());

    const unsigned char exact_text[] = {
        static_cast<unsigned char>('a'), 0u, static_cast<unsigned char>('b'),
        0xf0u, 0x9fu, 0x98u, 0x80u
    };
    esdb_value *text_value = nullptr;
    CHECK(esdb_value_create_text(
        ESDB_VALUE_UTF8, reinterpret_cast<const char *>(exact_text),
        sizeof(exact_text), &text_value, &error) == ESDB_OK);
    const esdb_value *text_refs[] = {text_value};
    QueryValueCapture text_capture{};
    CHECK(esdb_query(
        db, "SELECT ?", text_refs, 1u,
        capture_query_values, &text_capture, &query_rows, nullptr, &error) == ESDB_OK);
    CHECK(text_capture.types[0] == ESDB_VALUE_UTF8);
    CHECK(text_capture.sizes[0] == sizeof(exact_text));
    CHECK(text_capture.bytes[0].size() == sizeof(exact_text));
    CHECK(std::memcmp(
        text_capture.bytes[0].data(), exact_text, sizeof(exact_text)) == 0);

    const double positive_infinity = std::numeric_limits<double>::infinity();
    const double negative_infinity = -positive_infinity;
    esdb_value *positive_infinity_value = nullptr;
    esdb_value *negative_infinity_value = nullptr;
    esdb_value *negative_zero_value = nullptr;
    CHECK(esdb_value_create_double(
        positive_infinity, &positive_infinity_value, &error) == ESDB_OK);
    CHECK(esdb_value_create_double(
        negative_infinity, &negative_infinity_value, &error) == ESDB_OK);
    CHECK(esdb_value_create_double(-0.0, &negative_zero_value, &error) == ESDB_OK);
    const esdb_value *double_refs[3] = {
        positive_infinity_value, negative_infinity_value, negative_zero_value
    };
    QueryValueCapture double_capture{};
    CHECK(esdb_query(
        db, "SELECT ?, ?, ?", double_refs, 3u,
        capture_query_values, &double_capture, &query_rows, nullptr, &error) == ESDB_OK);
    CHECK(double_capture.types[0] == ESDB_VALUE_DOUBLE);
    CHECK(std::isinf(double_capture.reals[0]) && !std::signbit(double_capture.reals[0]));
    CHECK(std::isinf(double_capture.reals[1]) && std::signbit(double_capture.reals[1]));
    CHECK(double_capture.reals[2] == 0.0 && std::signbit(double_capture.reals[2]));

    esdb_value *nan_value = nullptr;
    CHECK(esdb_value_create_double(
        std::numeric_limits<double>::quiet_NaN(), &nan_value, &error) == ESDB_OK);
    const esdb_value *nan_refs[] = {nan_value};
    QueryValueCapture nan_capture{};
    CHECK(esdb_query(
        db, "SELECT ?", nan_refs, 1u,
        capture_query_values, &nan_capture, &query_rows, nullptr, &error) ==
        ESDB_ERR_UNSUPPORTED);
    CHECK(nan_capture.callbacks == 0u);

    const std::string injection = "x'); DROP TABLE query_probe; --";
    esdb_value *injection_value = nullptr;
    CHECK(esdb_value_create_text(
        ESDB_VALUE_UTF8, injection.data(),
        static_cast<uint64_t>(injection.size()), &injection_value, &error) == ESDB_OK);
    const esdb_value *injection_refs[] = {injection_value};
    QueryValueCapture injection_capture{};
    CHECK(esdb_query(
        db, "SELECT ?", injection_refs, 1u,
        capture_query_values, &injection_capture, &query_rows, nullptr, &error) == ESDB_OK);
    CHECK(injection_capture.bytes[0] == injection);
    Int64Capture table_count{};
    CHECK(esdb_query(
        db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='query_probe'",
        nullptr, 0u, capture_int64, &table_count, &query_rows, nullptr, &error) == ESDB_OK);
    CHECK(table_count.value == 1);

    const esdb_value *null_parameter_refs[] = {nullptr};
    CHECK(esdb_query(
        db, "SELECT ?", null_parameter_refs, 1u, nullptr, nullptr,
        nullptr, nullptr, &error) == ESDB_ERR_INVALID_ARGUMENT);
    const esdb_value *too_many_refs[] = {min_value, min_value};
    CHECK(esdb_query(
        db, "SELECT ?", too_many_refs, 2u, nullptr, nullptr,
        nullptr, nullptr, &error) == ESDB_ERR_INVALID_ARGUMENT);
    CHECK(esdb_query(
        db, "SELECT 1; /* non-whitespace tail */", nullptr, 0u, nullptr, nullptr,
        nullptr, nullptr, &error) == ESDB_ERR_INVALID_ARGUMENT);
    CHECK(esdb_query(
        db, "SELECT '", nullptr, 0u, nullptr, nullptr,
        nullptr, nullptr, &error) != ESDB_OK);
    const char invalid_sql_utf8[] = {
        'S', 'E', 'L', 'E', 'C', 'T', ' ', '\'',
        static_cast<char>(0xc3), '\'', '\0'
    };
    CHECK(esdb_query(
        db, invalid_sql_utf8, nullptr, 0u, nullptr, nullptr,
        nullptr, nullptr, &error) == ESDB_ERR_INVALID_ARGUMENT);

    CHECK(esdb_exec(
        db,
        "CREATE TRIGGER query_probe_abort BEFORE INSERT ON query_probe "
        "WHEN NEW.i = 99 BEGIN SELECT RAISE(ABORT, 'query failure'); END;",
        &error) == ESDB_OK);
    CHECK(esdb_query(
        db,
        "INSERT INTO query_probe(i,t,b,r,n,bo,i32v) "
        "VALUES(99,'x',X'00',0,NULL,0,0)",
        nullptr, 0u, nullptr, nullptr, nullptr, nullptr, &error) != ESDB_OK);

    QueryValueCapture invalid_text_capture{};
    CHECK(esdb_query(
        db, "SELECT CAST(X'C3' AS TEXT)", nullptr, 0u,
        capture_query_values, &invalid_text_capture, &query_rows, nullptr, &error) != ESDB_OK);
    CHECK(invalid_text_capture.callbacks == 0u);
    CHECK(esdb_query(
        db, "SELECT 1 AS i, 'two' AS t, X'00' AS b", nullptr, 0u,
        throwing_query_row, nullptr,
        nullptr, nullptr, &error) == ESDB_ERR_INTERNAL);

    Int64Capture recovery_capture{};
    CHECK(esdb_query(
        db, "SELECT 2", nullptr, 0u, capture_int64, &recovery_capture,
        &query_rows, nullptr, &error) == ESDB_OK);
    CHECK(recovery_capture.value == 2);
    CHECK(esdb_query(
        db, "BEGIN", nullptr, 0u, nullptr, nullptr, nullptr, nullptr, &error) == ESDB_OK);
    CHECK(esdb_query(
        db, "ROLLBACK", nullptr, 0u, nullptr, nullptr, nullptr, nullptr, &error) == ESDB_OK);

    for (esdb_value *value : distinct_values) esdb_value_destroy(value);
    esdb_value_destroy(min_value);
    esdb_value_destroy(max_value);
    esdb_value_destroy(text_value);
    esdb_value_destroy(positive_infinity_value);
    esdb_value_destroy(negative_infinity_value);
    esdb_value_destroy(negative_zero_value);
    esdb_value_destroy(nan_value);
    esdb_value_destroy(injection_value);

    CHECK(esdb_query(
        db, "SELECT ?", nullptr, 0u, nullptr, nullptr, nullptr, nullptr, &error) ==
        ESDB_ERR_INVALID_ARGUMENT);
    CHECK(esdb_query(
        db, "SELECT 1; SELECT 2", nullptr, 0u, nullptr, nullptr, nullptr, nullptr, &error) ==
        ESDB_ERR_INVALID_ARGUMENT);

    /* Raw typed SQL participates in the canonical ESDB transaction lifecycle. */
    esdb_value *rollback_id = nullptr;
    CHECK(esdb_value_create_int64(42, &rollback_id, &error) == ESDB_OK);
    const esdb_value *rollback_insert_params[7] = {
        rollback_id, query_params[1], query_params[2], query_params[3],
        query_params[4], query_params[5], query_params[6]
    };
    esdb_transaction *raw_transaction = nullptr;
    CHECK(esdb_begin(db, ESDB_TRANSACTION_IMMEDIATE, &raw_transaction, &error) == ESDB_OK);
    CHECK(raw_transaction != nullptr);
    CHECK(esdb_query(
        db,
        "INSERT INTO query_probe(i,t,b,r,n,bo,i32v) VALUES(?,?,?,?,?,?,?)",
        rollback_insert_params,
        7u,
        nullptr,
        nullptr,
        &query_rows,
        &query_changes,
        &error) == ESDB_OK);
    CHECK(query_changes == 1u);
    CHECK(esdb_rollback(raw_transaction, &error) == ESDB_OK);
    CHECK(esdb_transaction_active(raw_transaction) == 0);
    esdb_transaction_destroy(raw_transaction);

    const esdb_value *rollback_lookup_params[] = {rollback_id};
    Int64Capture rollback_count{};
    CHECK(esdb_query(
        db,
        "SELECT count(*) FROM query_probe WHERE i=?",
        rollback_lookup_params,
        1u,
        capture_int64,
        &rollback_count,
        &query_rows,
        &query_changes,
        &error) == ESDB_OK);
    CHECK(rollback_count.seen);
    CHECK(rollback_count.value == 0);
    CHECK(query_rows == 1u);
    CHECK(query_changes == 0u);

    esdb_value_destroy(rollback_id);
    esdb_value_destroy(empty_blob);
    for (esdb_value *value : query_params) esdb_value_destroy(value);

    esdb_value *theme = nullptr;
    const char *dark = "dark";
    CHECK(esdb_value_create_text(ESDB_VALUE_UTF8, dark, 4u, &theme, &error) == ESDB_OK);
    uint64_t rev1 = 0;
    CHECK(esdb_object_store_put(db, "settings", "theme", theme, &rev1, &error) == ESDB_OK);
    CHECK(rev1 > 0u);
    esdb_value_destroy(theme);

    esdb_value *read = nullptr;
    CHECK(esdb_object_store_get(db, "settings", "theme", &read, &error) == ESDB_OK);
    const void *payload = nullptr;
    uint64_t payload_size = 0;
    CHECK(esdb_value_type_of(read) == ESDB_VALUE_UTF8);
    CHECK(esdb_value_get_data(read, &payload, &payload_size) == ESDB_OK);
    CHECK(payload_size == 4u);
    CHECK(payload && std::memcmp(payload, "dark", 4) == 0);
    esdb_value_destroy(read);

    uint64_t count = 0;
    CHECK(esdb_object_store_count(db, "settings", &count, &error) == ESDB_OK);
    CHECK(count == 1u);

    int exists = 0;
    CHECK(esdb_object_store_exists(db, "settings", "theme", &exists, &error) == ESDB_OK);
    CHECK(exists == 1);

    esdb_object_subscription *subscription = nullptr;
    CHECK(esdb_object_subscribe(db, "settings", 0u, &subscription, &error) == ESDB_OK);
    ChangeCapture capture{};
    uint32_t polled = 0;
    CHECK(esdb_object_subscription_poll(subscription, 64u, capture_change, &capture, &polled, &error) == ESDB_OK);
    CHECK(polled == 1u);
    CHECK(capture.count == 1u);
    CHECK(esdb_object_subscription_revision(subscription) == rev1);

    int deleted = 0;
    uint64_t rev2 = 0;
    CHECK(esdb_object_store_delete(db, "settings", "theme", &deleted, &rev2, &error) == ESDB_OK);
    CHECK(deleted == 1);
    CHECK(rev2 > rev1);
    CHECK(esdb_object_subscription_poll(subscription, 64u, capture_change, &capture, &polled, &error) == ESDB_OK);
    CHECK(polled == 1u);
    CHECK(esdb_object_subscription_revision(subscription) == rev2);
    esdb_object_subscription_destroy(subscription);

    esdb_transaction *tx = nullptr;
    CHECK(esdb_begin(db, ESDB_TRANSACTION_IMMEDIATE, &tx, &error) == ESDB_OK);
    esdb_value *temp = nullptr;
    CHECK(esdb_value_create_int64(9223372036854770000LL, &temp, &error) == ESDB_OK);
    CHECK(esdb_object_store_put(db, "settings", "temporary", temp, nullptr, &error) == ESDB_OK);
    esdb_value_destroy(temp);
    CHECK(esdb_rollback(tx, &error) == ESDB_OK);
    esdb_transaction_destroy(tx);
    CHECK(esdb_object_store_exists(db, "settings", "temporary", &exists, &error) == ESDB_OK);
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
