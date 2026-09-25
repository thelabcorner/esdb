#include "generated/bridge/user_orm_bridge.h"
#include "generated/cpp/user_repository.hpp"
#include "generated/migrations/user_migrations.hpp"
#include "../../../bench/orm_bench_common.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

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
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(value[i]);
        out[i * 2u] = digits[(byte >> 4) & 0x0fu];
        out[i * 2u + 1u] = digits[byte & 0x0fu];
    }
    return out;
}

std::int32_t parse_handle(const std::string &wire) {
    if (wire.compare(0u, 5u, "U1:H:") != 0) return 0;
    char *end = NULL;
    const long value = std::strtol(wire.c_str() + 5u, &end, 10);
    if (end == NULL || *end != '\0' || value <= 0 || value > INT32_MAX) return 0;
    return static_cast<std::int32_t>(value);
}

void cleanup_db(const char *path) {
    std::remove(path);
    std::remove((std::string(path) + "-wal").c_str());
    std::remove((std::string(path) + "-shm").c_str());
}

double sample_native_find(esdb_generated_user::UserRepository &repository, int operations) {
    esdb_generated_user::UserRow row;
    const std::uint64_t start = esdb_orm_bench::now_ns();
    for (int i = 0; i < operations; ++i) {
        if (repository.find_by_id(1, &row) != 1) std::exit(20);
    }
    const std::uint64_t end = esdb_orm_bench::now_ns();
    return static_cast<double>(end - start) / static_cast<double>(operations);
}

double sample_bridge_find(esabi_value *args, int operations, std::size_t *wire_bytes) {
    esabi_value result{};
    const std::uint64_t start = esdb_orm_bench::now_ns();
    for (int i = 0; i < operations; ++i) {
        if (userFindById(args, 2, &result) != ESABI_OK) std::exit(21);
        const std::string wire = take_string(&result);
        if (wire.compare(0u, 6u, "U1:R:4") != 0) std::exit(22);
        *wire_bytes = wire.size();
    }
    const std::uint64_t end = esdb_orm_bench::now_ns();
    return static_cast<double>(end - start) / static_cast<double>(operations);
}

double sample_ping(esabi_value *dummy, int operations) {
    esabi_value result{};
    const std::uint64_t start = esdb_orm_bench::now_ns();
    for (int i = 0; i < operations; ++i) {
        if (ormPing(dummy, 1, &result) != ESABI_OK) std::exit(23);
        esabi_i32 value = 0;
        if (!esabi_value_get_i32(&result, &value) || value != 42) std::exit(24);
        esabi_value_set_undefined(&result);
    }
    const std::uint64_t end = esdb_orm_bench::now_ns();
    return static_cast<double>(end - start) / static_cast<double>(operations);
}

}  // namespace

int main() {
    const char *native_path = "esdb-orm-bridge-bench-native.sqlite";
    const char *bridge_path = "esdb-orm-bridge-bench-esabi.sqlite";
    cleanup_db(native_path);
    cleanup_db(bridge_path);

    esdb_open_options options;
    esdb_open_options_init(&options);
    options.journal_mode = ESDB_JOURNAL_WAL;
    options.synchronous = ESDB_SYNCHRONOUS_NORMAL;

    esdb_database *native_db = NULL;
    esdb_error error;
    esdb_error_clear(&error);
    if (esdb_open(native_path, &options, &native_db, &error) != ESDB_OK) {
        std::fprintf(stderr, "native esdb_open failed: %s\n", error.message);
        return 1;
    }
    esdb_error_clear(&error);
    if (esdb_generated_user_migrations::migrate(native_db, &error) != ESDB_OK) {
        std::fprintf(stderr, "native migration failed: %s\n", error.message);
        return 2;
    }

    esdb_generated_user::UserRepository repository(native_db);
    if (!repository.valid()) return 3;
    esdb_generated_user::UserInsert user;
    user.id = 1;
    user.name = "Ada";
    user.email = "ada@example.com";
    user.created_at = 1735689600;
    (void)repository.delete_by_id(1);
    if (repository.insert(user) != 1) return 4;

    char *signatures = ESInitialize(NULL, 0);
    if (signatures == NULL) return 5;

    esabi_value dummy = make_double(0.0);
    esabi_value result{};
    std::string bridge_hex = hex_ascii(bridge_path);
    esabi_value open_args[2] = {
        make_double(0.0),
        make_string(&bridge_hex[0])
    };
    if (ormOpen(open_args, 2, &result) != ESABI_OK) return 6;
    const std::int32_t handle = parse_handle(take_string(&result));
    if (handle <= 0) return 7;

    char id_lane[] = "i:1";
    char name_lane[] = "t:416461";
    char email_lane[] = "t:616461406578616D706C652E636F6D";
    char created_lane[] = "i:1735689600";
    esabi_value insert_args[5] = {
        make_i32(handle),
        make_string(id_lane),
        make_string(name_lane),
        make_string(email_lane),
        make_string(created_lane)
    };
    if (userInsert(insert_args, 5, &result) != ESABI_OK || take_string(&result) != "U1:C:1") return 8;

    esabi_value find_args[2] = {
        make_i32(handle),
        make_string(id_lane)
    };

    for (int i = 0; i < 5000; ++i) {
        esdb_generated_user::UserRow row;
        if (repository.find_by_id(1, &row) != 1) return 9;
        if (userFindById(find_args, 2, &result) != ESABI_OK) return 10;
        (void)take_string(&result);
        if (ormPing(&dummy, 1, &result) != ESABI_OK) return 11;
    }

    std::vector<double> native_samples;
    std::vector<double> bridge_samples;
    std::vector<double> ping_samples;
    native_samples.reserve(7);
    bridge_samples.reserve(7);
    ping_samples.reserve(7);
    std::size_t wire_bytes = 0u;

    for (int i = 0; i < 7; ++i) {
        native_samples.push_back(sample_native_find(repository, 50000));
        bridge_samples.push_back(sample_bridge_find(find_args, 50000, &wire_bytes));
        ping_samples.push_back(sample_ping(&dummy, 100000));
    }

    const double native_ns = esdb_orm_bench::median(native_samples);
    const double bridge_ns = esdb_orm_bench::median(bridge_samples);
    const double ping_ns = esdb_orm_bench::median(ping_samples);

    esabi_value close_arg = make_i32(handle);
    if (ormClose(&close_arg, 1, &result) != ESABI_OK) return 12;
    (void)take_string(&result);
    ESTerminate();

    esdb_close(native_db);
    cleanup_db(native_path);
    cleanup_db(bridge_path);

    std::printf(
        "{\"format\":\"esdb.orm-esabi-benchmark/v1\","
        "\"native_find_ns\":%.3f,"
        "\"bridge_find_ns\":%.3f,"
        "\"bridge_overhead_ns\":%.3f,"
        "\"bridge_ratio\":%.6f,"
        "\"ping_ns\":%.3f,"
        "\"row_wire_bytes\":%llu}\n",
        native_ns,
        bridge_ns,
        bridge_ns - native_ns,
        bridge_ns / native_ns,
        ping_ns,
        static_cast<unsigned long long>(wire_bytes));
    return 0;
}
