#include "cpp/user_repository.hpp"
#include "migrations/user_migrations.hpp"

#include <cstdint>
#include <string>

int main() {
    esdb_open_options options{};
    esdb_open_options_init(&options);

    esdb_error error{};
    esdb_database *database = nullptr;
    if (esdb_open(":memory:", &options, &database, &error) != ESDB_OK) {
        return 1;
    }

    if (esdb_generated_user_migrations::migrate(database, &error) != ESDB_OK) {
        esdb_close(database);
        return 2;
    }

    esdb_generated_user::UserRepository repository(database);
    if (!repository.valid()) {
        esdb_close(database);
        return 3;
    }

    esdb_generated_user::UserInsert row;
    row.id = 7;
    row.name = "Installed Package";
    row.email = "installed@example.com";
    row.created_at = INT64_C(1735689600);

    if (repository.insert(row) != 1) {
        esdb_close(database);
        return 4;
    }

    esdb_generated_user::UserRow found;
    if (repository.find_by_id(7, &found) != 1 ||
        found.id != 7 ||
        found.name != "Installed Package" ||
        found.email != "installed@example.com") {
        esdb_close(database);
        return 5;
    }

    if (repository.delete_by_id(7) != 1) {
        esdb_close(database);
        return 6;
    }

    esdb_close(database);
    return 0;
}
