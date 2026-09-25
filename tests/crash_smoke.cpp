#include <esdb/esdb.h>
#include <esdb/esdb_object_store.h>

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

const char *k_store = "crash";
const char *k_uncommitted = "uncommitted";
const char *k_committed = "committed";

void cleanup(const char *path) {
    std::remove(path);
    std::string wal = std::string(path) + "-wal";
    std::string shm = std::string(path) + "-shm";
    std::remove(wal.c_str());
    std::remove(shm.c_str());
}

bool open_wal(const char *path, esdb_database **out, esdb_error *error) {
    esdb_open_options options{};
    esdb_open_options_init(&options);
    options.journal_mode = ESDB_JOURNAL_WAL;
    options.synchronous = ESDB_SYNCHRONOUS_FULL;
    return esdb_open(path, &options, out, error) == ESDB_OK;
}

int child_uncommitted(const char *path) {
    esdb_error error{};
    esdb_database *db = nullptr;
    if (!open_wal(path, &db, &error)) return 11;

    esdb_transaction *tx = nullptr;
    if (esdb_begin(db, ESDB_TRANSACTION_IMMEDIATE, &tx, &error) != ESDB_OK) return 12;

    esdb_value *value = nullptr;
    if (esdb_value_create_int32(1, &value, &error) != ESDB_OK) return 13;
    if (esdb_object_store_put(db, k_store, k_uncommitted, value, nullptr, &error) != ESDB_OK) return 14;
    esdb_value_destroy(value);

    // Simulate abrupt process death: no rollback, transaction destruction, or
    // database close. SQLite must recover the uncommitted WAL transaction.
    std::_Exit(73);
}

int child_committed(const char *path) {
    esdb_error error{};
    esdb_database *db = nullptr;
    if (!open_wal(path, &db, &error)) return 21;

    esdb_value *value = nullptr;
    if (esdb_value_create_int32(2, &value, &error) != ESDB_OK) return 22;
    if (esdb_object_store_put(db, k_store, k_committed, value, nullptr, &error) != ESDB_OK) return 23;
    esdb_value_destroy(value);

    // The Store mutation is its own SQLite transaction here. Exit immediately
    // after the successful commit without a graceful database close.
    std::_Exit(74);
}

bool run_child(const char *exe, const char *mode, const char *path, int expected_exit) {
#if defined(_WIN32)
    const char *arguments[] = {"esdb_crash_smoke", mode, path, nullptr};
    const intptr_t rc = _spawnv(_P_WAIT, exe, arguments);
    if (rc != expected_exit) {
        std::fprintf(stderr, "child %s exited %lld (expected %d)\n",
                     mode, static_cast<long long>(rc), expected_exit);
        return false;
    }
    return true;
#else
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execl(exe, "esdb_crash_smoke", mode, path, static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_exit) {
        std::fprintf(stderr, "child %s status %d (expected exit %d)\n",
                     mode, status, expected_exit);
        return false;
    }
    return true;
#endif
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child-uncommitted") == 0) {
        return child_uncommitted(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--child-committed") == 0) {
        return child_committed(argv[2]);
    }
    if (argc != 1) {
        std::fprintf(stderr, "unexpected argc=%d", argc);
        for (int i = 0; i < argc; ++i) {
            std::fprintf(stderr, " argv[%d]=<%s>", i, argv[i] ? argv[i] : "(null)");
        }
        std::fprintf(stderr, "\n");
        return 2;
    }

    const char *path = "esdb-crash-smoke.sqlite";
    cleanup(path);

    esdb_error error{};
    esdb_database *db = nullptr;
    if (!open_wal(path, &db, &error)) return 3;
    if (esdb_object_store_ensure(db, k_store, &error) != ESDB_OK) return 4;
    uint64_t baseline_revision = 0;
    if (esdb_object_store_revision(db, &baseline_revision, &error) != ESDB_OK) return 5;
    esdb_close(db);
    db = nullptr;

    if (!run_child(argv[0], "--child-uncommitted", path, 73)) return 6;

    if (!open_wal(path, &db, &error)) return 7;
    int exists = 1;
    if (esdb_object_store_exists(db, k_store, k_uncommitted, &exists, &error) != ESDB_OK) return 8;
    if (exists != 0) return 9;
    uint64_t recovered_revision = 0;
    if (esdb_object_store_revision(db, &recovered_revision, &error) != ESDB_OK) return 10;
    if (recovered_revision != baseline_revision) return 11;
    if (esdb_integrity_check(db, 1, &error) != ESDB_OK) return 12;
    esdb_close(db);
    db = nullptr;

    if (!run_child(argv[0], "--child-committed", path, 74)) return 13;

    if (!open_wal(path, &db, &error)) return 14;
    exists = 0;
    if (esdb_object_store_exists(db, k_store, k_committed, &exists, &error) != ESDB_OK) return 15;
    if (exists != 1) return 16;
    if (esdb_object_store_revision(db, &recovered_revision, &error) != ESDB_OK) return 17;
    if (recovered_revision <= baseline_revision) return 18;
    if (esdb_integrity_check(db, 1, &error) != ESDB_OK) return 19;
    esdb_close(db);

    cleanup(path);
    std::puts("ESDB crash smoke: PASS");
    return 0;
}
