/*
 * ESDB ORM vertical slice: native runtime smoke.
 *
 * The Drizzle schema is the authoring source. Drizzle Kit owns shipped DDL;
 * generated/migrations/user_migrations.hpp is the only migration code consumed
 * here. No DDL or arbitrary SQL is duplicated in this file.
 */

#include "generated/cpp/user_repository.hpp"
#include "generated/migrations/user_migrations.hpp"

#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

int fail(const char *stage, const char *detail) {
    std::fprintf(stderr, "%s failed: %s\n", stage, detail != NULL ? detail : "");
    return 1;
}

int fail_code(const char *stage, int code, const char *detail) {
    std::fprintf(
        stderr,
        "%s failed (%d): %s\n",
        stage,
        code,
        detail != NULL ? detail : "");
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "esdb-orm-user-example.sqlite";
    esdb_open_options options;
    esdb_database *database = NULL;
    esdb_error error;

    esdb_open_options_init(&options);
    options.journal_mode = ESDB_JOURNAL_WAL;
    options.synchronous = ESDB_SYNCHRONOUS_FULL;

    esdb_error_clear(&error);
    if (esdb_open(path, &options, &database, &error) != ESDB_OK) {
        return fail("esdb_open", error.message);
    }

    esdb_error_clear(&error);
    if (esdb_generated_user_migrations::migrate(database, &error) != ESDB_OK) {
        esdb_close(database);
        return fail("generated migrate", error.message);
    }

    /* The generated migration chain must be idempotent at its target version. */
    esdb_error_clear(&error);
    if (esdb_generated_user_migrations::migrate(database, &error) != ESDB_OK) {
        esdb_close(database);
        return fail("generated migrate (idempotent rerun)", error.message);
    }

    std::uint32_t user_version = 0u;
    esdb_error_clear(&error);
    if (esdb_user_version_get(database, &user_version, &error) != ESDB_OK) {
        esdb_close(database);
        return fail("esdb_user_version_get", error.message);
    }
    if (user_version != esdb_generated_user_migrations::target_version()) {
        std::fprintf(
            stderr,
            "user_version mismatch: expected %u, got %u\n",
            esdb_generated_user_migrations::target_version(),
            user_version);
        esdb_close(database);
        return 1;
    }

    if (std::string(esdb_generated_user::expected_ir_hash()) !=
        std::string(esdb_generated_user_migrations::expected_ir_hash())) {
        esdb_close(database);
        return fail("IR hash coherence", "repository and migration artifacts disagree");
    }

    esdb_generated_user::UserRepository repository(database);
    if (!repository.valid()) {
        esdb_close(database);
        return fail("repository prepare", repository.last_error_message());
    }

    const std::int64_t max_id = (std::numeric_limits<std::int64_t>::max)();

    /* Make the smoke repeatable without owning arbitrary SQL. */
    (void)repository.delete_by_id(1);
    (void)repository.delete_by_id(max_id);

    esdb_generated_user::UserInsert ada;
    ada.id = 1;
    ada.name = "Ada Lovelace";
    ada.email = "ada@example.com";
    ada.created_at = 1735689600;

    int rc = repository.insert(ada);
    if (rc != 1) {
        esdb_close(database);
        return fail_code("insert Ada", rc, repository.last_error_message());
    }

    esdb_generated_user::UserInsert boundary;
    boundary.id = max_id;
    boundary.name = "Int64 Boundary";
    boundary.email = "int64-max@example.com";
    boundary.created_at = max_id;

    rc = repository.insert(boundary);
    if (rc != 1) {
        esdb_close(database);
        return fail_code("insert int64 boundary", rc, repository.last_error_message());
    }

    esdb_generated_user::UserRow row;
    rc = repository.find_by_id(1, &row);
    if (rc != 1 || row.id != 1 || row.name != "Ada Lovelace" ||
        row.email != "ada@example.com" || row.created_at != 1735689600) {
        esdb_close(database);
        return fail_code("find_by_id Ada", rc, repository.last_error_message());
    }

    rc = repository.find_by_id(max_id, &row);
    if (rc != 1 || row.id != max_id || row.created_at != max_id) {
        esdb_close(database);
        return fail_code("find_by_id int64 boundary", rc, repository.last_error_message());
    }

    rc = repository.update_name(1, "Ada King");
    if (rc != 1) {
        esdb_close(database);
        return fail_code("update_name", rc, repository.last_error_message());
    }

    rc = repository.find_by_id(1, &row);
    if (rc != 1 || row.name != "Ada King") {
        esdb_close(database);
        return fail_code("find after update", rc, repository.last_error_message());
    }

    std::vector<esdb_generated_user::UserRow> rows;
    rc = repository.list(10, 0, &rows);
    if (rc < 2 || rows.size() < 2u) {
        esdb_close(database);
        return fail_code("list", rc, repository.last_error_message());
    }

    rc = repository.delete_by_id(1);
    if (rc != 1) {
        esdb_close(database);
        return fail_code("delete Ada", rc, repository.last_error_message());
    }
    rc = repository.delete_by_id(max_id);
    if (rc != 1) {
        esdb_close(database);
        return fail_code("delete int64 boundary", rc, repository.last_error_message());
    }

    esdb_close(database);
    std::printf(
        "ESDB ORM user smoke PASS (user_version=%u, ir=%s)\n",
        user_version,
        esdb_generated_user::expected_ir_hash());
    return 0;
}
