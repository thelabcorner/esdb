#include <esdb/esdb.h>
#include <esdb/sqlite3.h>

#include <string.h>

int main(void) {
    esdb_open_options options;
    esdb_error error;
    esdb_database *database = NULL;

    esdb_open_options_init(&options);
    esdb_error_clear(&error);

    if (esdb_open(":memory:", &options, &database, &error) != ESDB_OK) {
        return 1;
    }

    sqlite3 *native = (sqlite3 *)esdb_native_handle(database);
    if (native == NULL || strcmp(sqlite3_libversion(), esdb_sqlite_version()) != 0) {
        esdb_close(database);
        return 2;
    }

    esdb_close(database);
    return 0;
}
