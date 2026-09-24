#include <esdb/esdb.h>
#include <esdb/sqlite3.h>

#include <cstring>

int main() {
    esdb_open_options options{};
    esdb_open_options_init(&options);

    esdb_error error{};
    esdb_database *database = nullptr;
    if (esdb_open(":memory:", &options, &database, &error) != ESDB_OK) {
        return 1;
    }

    sqlite3 *native = static_cast<sqlite3 *>(esdb_native_handle(database));
    if (!native || std::strcmp(sqlite3_libversion(), esdb_sqlite_version()) != 0) {
        esdb_close(database);
        return 2;
    }

    esdb_close(database);
    return 0;
}
