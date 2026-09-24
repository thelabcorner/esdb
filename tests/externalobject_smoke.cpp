#include "esdb_externalobject_abi.h"

#include <atomic>
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
    CHECK(std::strstr(signatures, "openStaged_f") != nullptr);
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
    CHECK(take_string(version_result) == "0.1.0");

    esabi_value sqlite_result{};
    CHECK(sqliteVersion(&dummy, 1, &sqlite_result) == ESABI_OK);
    CHECK(take_string(sqlite_result) == "3.53.4");

    char mutable_path[] = "esdb-externalobject-smoke.sqlite";
    esabi_value path_arg = make_string(mutable_path);
    CHECK(stage(&path_arg, 1, &result) == ESABI_OK);
    int staged = 0;
    CHECK(esabi_value_get_bool(&result, &staged) != 0);
    CHECK(staged == 1);

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
