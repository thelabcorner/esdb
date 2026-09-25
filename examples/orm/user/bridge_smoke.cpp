/*
 * Concrete ESABI bridge smoke for the generated User ORM contract.
 *
 * This executes the real bridge functions directly through ESABI values. It is
 * intentionally host-independent: Illustrator host loading remains a separate
 * integration gate, while this verifies bridge lifecycle, migrations, strict
 * wire parsing, CRUD, and stale-handle rejection against real ESDB Runtime.
 */

#include "generated/bridge/user_orm_bridge.h"
#include "generated/cpp/user_repository.hpp"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        failures += 1; \
    } \
} while (0)

esabi_value make_double(double value) {
    esabi_value out{};
    esabi_value_set_double(&out, value);
    return out;
}

esabi_value make_i32(esabi_i32 value) {
    esabi_value out{};
    esabi_value_set_i32(&out, value);
    return out;
}

esabi_value make_string(char *value) {
    esabi_value out{};
    esabi_value_set_string(&out, value);
    return out;
}

std::string take_string(esabi_value *value) {
    const char *text = esabi_value_get_string(value);
    std::string out = text != NULL ? text : "";
    if (text != NULL) {
        ESFreeMem(value->payload.string_value);
    }
    esabi_value_set_undefined(value);
    return out;
}

std::string hex_ascii(const std::string &value) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.resize(value.size() * 2u);
    for (std::size_t i = 0; i < value.size(); i += 1u) {
        const unsigned char byte = static_cast<unsigned char>(value[i]);
        out[i * 2u] = digits[(byte >> 4) & 0x0fu];
        out[i * 2u + 1u] = digits[byte & 0x0fu];
    }
    return out;
}

std::int32_t parse_handle(const std::string &wire) {
    const char prefix[] = "U1:H:";
    if (wire.compare(0u, sizeof(prefix) - 1u, prefix) != 0) {
        return 0;
    }
    char *end = NULL;
    const long parsed = std::strtol(wire.c_str() + sizeof(prefix) - 1u, &end, 10);
    if (end == NULL || *end != '\0' || parsed <= 0 || parsed > INT32_MAX) {
        return 0;
    }
    return static_cast<std::int32_t>(parsed);
}

void cleanup_db(const char *path) {
    std::remove(path);
    std::remove((std::string(path) + "-wal").c_str());
    std::remove((std::string(path) + "-shm").c_str());
}

}  // namespace

int main() {
    const char *db_path = "esdb-orm-user-bridge-smoke.sqlite";
    cleanup_db(db_path);

    char *signatures = ESInitialize(NULL, 0);
    CHECK(signatures != NULL);
    if (signatures != NULL) {
        CHECK(std::strstr(signatures, "ormOpen_fs") != NULL);
        CHECK(std::strstr(signatures, "userInsert_dssss") != NULL);
        CHECK(std::strstr(signatures, "userUpdateName_dss") != NULL);
    }
    CHECK(ESGetVersion() == 1);

    esabi_value dummy = make_double(0.0);
    esabi_value result{};

    CHECK(ormPing(&dummy, 1, &result) == ESABI_OK);
    esabi_i32 ping_value = 0;
    CHECK(esabi_value_get_i32(&result, &ping_value) != 0);
    CHECK(ping_value == 42);

    CHECK(ormVersion(&dummy, 1, &result) == ESABI_OK);
    CHECK(take_string(&result) == esdb_generated_user::expected_ir_hash());

    std::string path_hex_text = hex_ascii(db_path);
    esabi_value open_args[2] = {
        make_double(0.0),
        make_string(&path_hex_text[0])
    };
    CHECK(ormOpen(open_args, 2, &result) == ESABI_OK);
    const std::string open_wire = take_string(&result);
    const std::int32_t handle = parse_handle(open_wire);
    CHECK(handle > 0);

    char id_lane[] = "i:1";
    char name_lane[] = "t:416461"; /* Ada */
    char email_lane[] = "t:616461406578616D706C652E636F6D"; /* ada@example.com */
    char created_lane[] = "i:1735689600";
    esabi_value insert_args[5] = {
        make_i32(handle),
        make_string(id_lane),
        make_string(name_lane),
        make_string(email_lane),
        make_string(created_lane)
    };
    CHECK(userInsert(insert_args, 5, &result) == ESABI_OK);
    CHECK(take_string(&result) == "U1:C:1");

    esabi_value find_args[2] = {
        make_i32(handle),
        make_string(id_lane)
    };
    CHECK(userFindById(find_args, 2, &result) == ESABI_OK);
    std::string row = take_string(&result);
    CHECK(row.compare(0u, 6u, "U1:R:4") == 0);
    CHECK(row.find("|t|416461") != std::string::npos);

    char updated_name_lane[] = "t:416461204B696E67"; /* Ada King */
    esabi_value update_args[3] = {
        make_i32(handle),
        make_string(id_lane),
        make_string(updated_name_lane)
    };
    CHECK(userUpdateName(update_args, 3, &result) == ESABI_OK);
    CHECK(take_string(&result) == "U1:C:1");

    CHECK(userFindById(find_args, 2, &result) == ESABI_OK);
    row = take_string(&result);
    CHECK(row.find("|t|416461204B696E67") != std::string::npos);

    char limit_lane[] = "i:10";
    char offset_lane[] = "i:0";
    esabi_value list_args[3] = {
        make_i32(handle),
        make_string(limit_lane),
        make_string(offset_lane)
    };
    CHECK(userList(list_args, 3, &result) == ESABI_OK);
    const std::string list_wire = take_string(&result);
    CHECK(list_wire.compare(0u, 6u, "U1:L:1") == 0);

    /* The wire parser rejects non-canonical integer text before SQL. */
    char bad_id_lane[] = "i:01";
    esabi_value bad_find_args[2] = {
        make_i32(handle),
        make_string(bad_id_lane)
    };
    CHECK(userFindById(bad_find_args, 2, &result) == ESABI_OK);
    CHECK(take_string(&result).compare(0u, 5u, "U1:E:") == 0);

    CHECK(userDeleteById(find_args, 2, &result) == ESABI_OK);
    CHECK(take_string(&result) == "U1:C:1");

    CHECK(userFindById(find_args, 2, &result) == ESABI_OK);
    CHECK(take_string(&result) == "U1:N");

    esabi_value close_arg = make_i32(handle);
    CHECK(ormClose(&close_arg, 1, &result) == ESABI_OK);
    CHECK(take_string(&result) == "U1:C:1");

    /* Closed generation-tagged handles must never revive. */
    CHECK(userFindById(find_args, 2, &result) == ESABI_OK);
    CHECK(take_string(&result).compare(0u, 5u, "U1:E:") == 0);

    /* Reopen proves the generated migration wrapper is target-version idempotent. */
    CHECK(ormOpen(open_args, 2, &result) == ESABI_OK);
    const std::int32_t reopened = parse_handle(take_string(&result));
    CHECK(reopened > 0);
    CHECK(reopened != handle);

    esabi_value reopened_close = make_i32(reopened);
    CHECK(ormClose(&reopened_close, 1, &result) == ESABI_OK);
    CHECK(take_string(&result) == "U1:C:1");

    ESTerminate();
    cleanup_db(db_path);

    if (failures != 0) {
        std::fprintf(stderr, "ESDB ORM bridge smoke: %d failure(s)\n", failures);
        return 1;
    }

    std::printf("ESDB ORM concrete bridge smoke PASS\n");
    return 0;
}
