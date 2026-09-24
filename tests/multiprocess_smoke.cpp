#include <esdb/esdb.h>
#include <esdb/esdb_store.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr int k_writes_per_process = 32;
const char *k_store = "multiprocess";

void cleanup(const char *path) {
    std::remove(path);
    std::string wal = std::string(path) + "-wal";
    std::string shm = std::string(path) + "-shm";
    std::remove(wal.c_str());
    std::remove(shm.c_str());
}

bool open_database(
    const char *path,
    bool establish_wal,
    esdb_database **out,
    esdb_error *error) {
    esdb_open_options options{};
    esdb_open_options_init(&options);
    options.journal_mode =
        establish_wal ? ESDB_JOURNAL_WAL : ESDB_JOURNAL_UNCHANGED;
    options.synchronous = ESDB_SYNCHRONOUS_FULL;
    options.busy_timeout_ms = 5000u;
    if (esdb_open(path, &options, out, error) != ESDB_OK) return false;

    esdb_database_health health{};
    health.struct_size = sizeof(health);
    if (esdb_database_health_get(*out, &health, error) != ESDB_OK ||
        health.journal_mode != ESDB_JOURNAL_WAL) {
        esdb_close(*out);
        *out = nullptr;
        return false;
    }
    return true;
}

int writer(const char *path, const char *prefix) {
    esdb_error error{};
    esdb_database *db = nullptr;
    if (!open_database(path, false, &db, &error)) {
        std::fprintf(
            stderr,
            "writer %s open failed: status=%u sqlite=%d ext=%d message=%s\n",
            prefix,
            static_cast<unsigned>(error.status),
            error.sqlite_code,
            error.sqlite_extended_code,
            error.message);
        return 20;
    }

    esdb_value *value = nullptr;
    if (esdb_value_create_int32(1, &value, &error) != ESDB_OK) {
        esdb_close(db);
        return 21;
    }

    for (int index = 0; index < k_writes_per_process; ++index) {
        char key[64]{};
        std::snprintf(key, sizeof(key), "%s-%02d", prefix, index);
        const esdb_status put_status =
            esdb_store_put(db, k_store, key, value, nullptr, &error);
        if (put_status != ESDB_OK) {
            std::fprintf(
                stderr,
                "writer %s key %s failed: status=%u sqlite=%d ext=%d message=%s\n",
                prefix,
                key,
                static_cast<unsigned>(put_status),
                error.sqlite_code,
                error.sqlite_extended_code,
                error.message);
            esdb_value_destroy(value);
            esdb_close(db);
            return 22;
        }
    }

    esdb_value_destroy(value);
    esdb_close(db);
    return 0;
}

struct Child {
#if defined(_WIN32)
    intptr_t handle = -1;
#else
    pid_t pid = -1;
#endif
};

bool spawn_writer(
    const char *exe,
    const char *path,
    const char *prefix,
    Child &out) {
#if defined(_WIN32)
    const char *arguments[] = {
        "esdb_multiprocess_smoke",
        "--writer",
        path,
        prefix,
        nullptr
    };
    out.handle = _spawnv(_P_NOWAIT, exe, arguments);
    return out.handle != -1;
#else
    out.pid = fork();
    if (out.pid < 0) return false;
    if (out.pid == 0) {
        execl(
            exe,
            "esdb_multiprocess_smoke",
            "--writer",
            path,
            prefix,
            static_cast<char *>(nullptr));
        _exit(127);
    }
    return true;
#endif
}

bool wait_child(Child &child) {
#if defined(_WIN32)
    int status = -1;
    const intptr_t result = _cwait(&status, child.handle, 0);
    if (result == -1 || status != 0) {
        std::fprintf(stderr, "writer process wait failed: result=%lld status=%d\n",
                     static_cast<long long>(result), status);
        return false;
    }
    return true;
#else
    int status = 0;
    if (waitpid(child.pid, &status, 0) != child.pid) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "writer process status=%d\n", status);
        return false;
    }
    return true;
#endif
}

struct RevisionAudit {
    std::uint64_t previous = 0u;
    std::uint32_t count = 0u;
    bool monotonic = true;
};

int audit_change(const esdb_change *change, void *user_data) {
    auto *audit = static_cast<RevisionAudit *>(user_data);
    if (!change || change->revision <= audit->previous) {
        audit->monotonic = false;
        return 1;
    }
    audit->previous = change->revision;
    ++audit->count;
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::strcmp(argv[1], "--writer") == 0) {
        return writer(argv[2], argv[3]);
    }
    if (argc != 1) return 2;

    const char *path = "esdb-multiprocess-smoke.sqlite";
    cleanup(path);

    esdb_error error{};
    esdb_database *db = nullptr;
    if (!open_database(path, true, &db, &error)) return 3;
    /*
     * Deliberately leave the Store schema uninitialized. The two child
     * processes race through first-use initialization and then continue as
     * concurrent WAL writers.
     */
    esdb_close(db);
    db = nullptr;

    Child first{};
    Child second{};
    if (!spawn_writer(argv[0], path, "A", first)) return 5;
    if (!spawn_writer(argv[0], path, "B", second)) {
        (void)wait_child(first);
        return 6;
    }

    const bool first_ok = wait_child(first);
    const bool second_ok = wait_child(second);
    if (!first_ok || !second_ok) return 7;

    if (!open_database(path, true, &db, &error)) return 8;

    std::uint64_t count = 0u;
    if (esdb_store_count(db, k_store, &count, &error) != ESDB_OK) return 9;
    if (count != static_cast<std::uint64_t>(k_writes_per_process * 2)) return 10;

    std::uint64_t revision = 0u;
    if (esdb_store_revision(db, &revision, &error) != ESDB_OK) return 11;
    if (revision != count) return 12;

    RevisionAudit audit{};
    std::uint64_t last = 0u;
    std::uint32_t delivered = 0u;
    if (esdb_store_changes_since(
            db,
            k_store,
            0u,
            0u,
            audit_change,
            &audit,
            &last,
            &delivered,
            &error) != ESDB_OK) {
        return 13;
    }
    if (!audit.monotonic || audit.count != count || delivered != count) return 14;
    if (last != revision) return 15;

    if (esdb_integrity_check(db, 1, &error) != ESDB_OK) return 16;
    esdb_close(db);

    cleanup(path);
    std::puts("ESDB multiprocess smoke: PASS");
    return 0;
}
