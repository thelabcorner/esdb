/*
 * ESDB ORM "user" example: concrete ESABI ExternalObject bridge.
 *
 * Implements the named-operation contract generated in
 * generated/bridge/user_orm_bridge.h on top of ESDB and the generated C++
 * repository bindings (generated/cpp/user_repository.hpp). It never accepts
 * SQL: every caller-supplied value is parsed with the strict U1 lane grammar
 * and reaches SQLite only through generated prepared statements.
 *
 * ESABI rules honored here:
 *   - fixed signatures from the generated contract, including the dummy
 *     double argument on no-value operations (a measured host requirement);
 *   - every returned string is malloc()'ed and released by ESFreeMem;
 *   - business failures are wire-error strings ("U1:E:...") with non-negative
 *     statuses; the exported function still returns ESABI_OK. Only host-level
 *     failures (bad retval, allocation failure) return a negative status.
 *
 * Parameter mapping is semantic: each operation declares its parameters by
 * name, values are parsed into a name-keyed vector, and repository methods are
 * called with values looked up by name. Positional wire order is validated
 * against the generated signature, but it is never assumed to match SQL
 * placeholder order inside the generated statements (UPDATE is the case in
 * point: SQL binds SET before WHERE while the API takes the key first).
 *
 * Lifecycle: ormOpen() owns an esdb_database plus a UserRepository; ormClose()
 * finalizes the repository before closing the database and advances the slot
 * generation so stale handles are rejected. ESTerminate() deterministically
 * closes every remaining handle.
 */

#include "../generated/bridge/user_orm_bridge.h"

#include "../generated/cpp/user_repository.hpp"

#include "../generated/migrations/user_migrations.hpp"
#include "user_orm_wire.hpp"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

/* ---- handle registry ---- */

constexpr std::uint32_t kSlotBits = 6u;
/* Low zero is reserved as an invalid-handle sentinel, so N slot bits encode
 * 1..(2^N-1), not 2^N usable slots. */
constexpr std::uint32_t kSlotCount = (1u << kSlotBits) - 1u;
constexpr std::uint32_t kGenerationMax =
    static_cast<std::uint32_t>(INT32_MAX) >> kSlotBits;

struct Slot {
    esdb_database *database;
    esdb_generated_user::UserRepository *repository;
    std::uint32_t generation;
    bool in_use;
    bool retired;
};

Slot g_slots[kSlotCount];
std::mutex g_mutex;

struct LastErrorState {
    bool set;
    int status;
    std::string message;
};

LastErrorState g_last_error = {false, 0, std::string()};

std::int32_t encode_handle(std::uint32_t slot_index, std::uint32_t generation) {
    return static_cast<std::int32_t>(
        (generation << kSlotBits) | (slot_index + 1u));
}

bool decode_handle(
    std::int32_t token,
    std::uint32_t *out_slot_index,
    std::uint32_t *out_generation) {
    if (token <= 0) {
        return false;
    }
    const std::uint32_t raw = static_cast<std::uint32_t>(token);
    const std::uint32_t encoded_slot = raw & ((1u << kSlotBits) - 1u);
    if (encoded_slot == 0u || encoded_slot > kSlotCount) {
        return false;
    }
    const std::uint32_t generation = raw >> kSlotBits;
    if (generation == 0u || generation > kGenerationMax) {
        return false;
    }
    *out_slot_index = encoded_slot - 1u;
    *out_generation = generation;
    return true;
}

Slot *slot_for_handle(std::int32_t token) {
    std::uint32_t slot_index = 0;
    std::uint32_t generation = 0;
    if (!decode_handle(token, &slot_index, &generation)) {
        return NULL;
    }
    Slot &slot = g_slots[slot_index];
    if (!slot.in_use || slot.retired || slot.database == NULL ||
        slot.repository == NULL || slot.generation != generation) {
        return NULL;
    }
    return &slot;
}

void release_slot(Slot *slot) {
    if (slot->repository != NULL) {
        delete slot->repository;
        slot->repository = NULL;
    }
    if (slot->database != NULL) {
        esdb_close(slot->database);
        slot->database = NULL;
    }
    if (slot->generation >= kGenerationMax) {
        /* Never wrap a generation: a wrapped handle could revive stale tokens. */
        slot->retired = true;
    } else {
        slot->generation += 1u;
    }
    slot->in_use = false;
}

/* ---- error state and replies (callers hold g_mutex) ---- */

void set_last_error(int status, const std::string &message) {
    g_last_error.set = true;
    g_last_error.status = status;
    g_last_error.message = message;
}

esabi_error emit_string(esabi_value *retval, const std::string &wire) {
    if (retval == NULL) {
        return ESABI_ERR_BAD_ARGUMENTS;
    }
    char *copy = static_cast<char *>(std::malloc(wire.size() + 1u));
    if (copy == NULL) {
        return ESABI_ERR_OUT_OF_MEMORY;
    }
    std::memcpy(copy, wire.data(), wire.size());
    copy[wire.size()] = '\0';
    esabi_value_set_string(retval, copy);
    return ESABI_OK;
}

esabi_error emit_error_reply(esabi_value *retval, int status, const std::string &message) {
    set_last_error(status, message);
    return emit_string(retval, esdb_orm_user::wire::reply_error(status, message));
}

template <typename Fn>
esabi_error guarded(Fn fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc &) {
        return ESABI_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return ESABI_ERR_INTERNAL;
    }
}

/* ---- strict U1 parameter lanes ---- */

enum class Lane { Integer, Text, Real, Blob };

struct ParamSpec {
    const char *name;
    Lane lane;
};

struct ParsedValue {
    bool is_null;
    Lane lane;
    std::int64_t integer;
    double real;
    std::string text;

    ParsedValue() : is_null(false), lane(Lane::Integer), integer(0), real(0.0), text() {}
};

bool parse_param(const char *text, Lane lane, ParsedValue *out) {
    if (text == NULL || out == NULL) {
        return false;
    }
    out->is_null = false;
    out->lane = lane;
    out->integer = 0;
    out->real = 0.0;
    out->text.clear();

    if (std::strcmp(text, "n") == 0) {
        out->is_null = true;
        return true;
    }

    switch (lane) {
        case Lane::Integer: {
            if (std::strncmp(text, "i:", 2u) != 0) {
                return false;
            }
            return esdb_orm_user::wire::canonical_int64(text + 2u, &out->integer);
        }
        case Lane::Text: {
            if (std::strncmp(text, "t:", 2u) != 0) {
                return false;
            }
            std::vector<unsigned char> bytes;
            if (!esdb_orm_user::wire::hex_decode(text + 2u, &bytes)) {
                return false;
            }
            if (!esdb_orm_user::wire::utf8_valid(
                    bytes.empty() ? NULL : &bytes[0], bytes.size())) {
                return false;
            }
            out->text.assign(bytes.begin(), bytes.end());
            return true;
        }
        case Lane::Real: {
            if (std::strncmp(text, "r:", 2u) != 0 || text[2] == '\0') {
                return false;
            }
            errno = 0;
            char *end = NULL;
            const double value = std::strtod(text + 2u, &end);
            if (errno == ERANGE || end == NULL || *end != '\0') {
                return false;
            }
            if (!(value == value) || value > std::numeric_limits<double>::max() ||
                value < -std::numeric_limits<double>::max()) {
                return false; /* reject NaN and infinities */
            }
            out->real = value;
            return true;
        }
        case Lane::Blob: {
            if (std::strncmp(text, "b:", 2u) != 0) {
                return false;
            }
            std::vector<unsigned char> bytes;
            if (!esdb_orm_user::wire::hex_decode(text + 2u, &bytes)) {
                return false;
            }
            out->text.assign(bytes.begin(), bytes.end());
            return true;
        }
    }
    return false;
}

void set_argument_error(
    std::string *out_message,
    const ParamSpec *specs,
    std::size_t index) {
    *out_message = std::string("parameter \"") + specs[index].name +
        "\" does not match its U1 lane";
}

bool parse_arguments(
    const esabi_value *argv,
    esabi_long argc,
    const ParamSpec *specs,
    std::size_t count,
    std::int32_t *out_handle,
    std::vector<ParsedValue> *out_values,
    std::string *out_message) {
    if (argc < 0 || static_cast<std::size_t>(argc) != count + 1u) {
        *out_message = "argument count mismatch";
        return false;
    }

    esabi_i32 handle = 0;
    if (esabi_arg_get_i32(argv, argc, 0, &handle)) {
        /* strict form */
    } else {
        /* tolerate an integral double host marshaling of the same handle */
        double as_double = 0.0;
        if (!esabi_arg_get_double(argv, argc, 0, &as_double) ||
            !(as_double >= static_cast<double>(INT32_MIN) &&
              as_double <= static_cast<double>(INT32_MAX)) ||
            as_double != static_cast<double>(static_cast<std::int32_t>(as_double))) {
            *out_message = "handle argument must be an i32";
            return false;
        }
        handle = static_cast<esabi_i32>(as_double);
    }

    out_values->clear();
    out_values->reserve(count);
    for (std::size_t i = 0; i < count; i += 1u) {
        const char *text = esabi_arg_get_string(
            argv, argc, static_cast<esabi_long>(i + 1u));
        ParsedValue value;
        if (!parse_param(text, specs[i].lane, &value)) {
            set_argument_error(out_message, specs, i);
            return false;
        }
        if (value.is_null) {
            *out_message = std::string("parameter \"") + specs[i].name +
                "\" is not nullable";
            return false;
        }
        out_values->push_back(value);
    }
    *out_handle = handle;
    return true;
}

/*
 * Semantic parameter lookup: operation handlers resolve each value by the
 * generated parameter name, never by its position in the wire or in any SQL
 * placeholder list.
 */
const ParsedValue *find_value(
    const std::vector<ParsedValue> &values,
    const ParamSpec *specs,
    const char *name) {
    for (std::size_t i = 0; i < values.size(); i += 1u) {
        if (std::strcmp(specs[i].name, name) == 0) {
            return &values[i];
        }
    }
    return NULL;
}

/* ---- row encoding ---- */

std::string row_wire(const esdb_generated_user::UserRow &row) {
    std::string out = "U1:R:4";
    esdb_orm_user::wire::append_integer_field(&out, row.id);
    esdb_orm_user::wire::append_field(&out, 't', row.name);
    esdb_orm_user::wire::append_field(&out, 't', row.email);
    esdb_orm_user::wire::append_integer_field(&out, row.created_at);
    return out;
}

std::string rows_wire(const std::vector<esdb_generated_user::UserRow> &rows) {
    std::string out = "U1:L:" +
        esdb_orm_user::wire::int64_to_decimal(
            static_cast<std::int64_t>(rows.size()));
    for (std::size_t i = 0; i < rows.size(); i += 1u) {
        out += "|4";
        esdb_orm_user::wire::append_integer_field(&out, rows[i].id);
        esdb_orm_user::wire::append_field(&out, 't', rows[i].name);
        esdb_orm_user::wire::append_field(&out, 't', rows[i].email);
        esdb_orm_user::wire::append_integer_field(&out, rows[i].created_at);
    }
    return out;
}

std::string repository_error(const Slot &slot, const char *operation) {
    const char *detail = slot.repository != NULL
        ? slot.repository->last_error_message() : "repository is not available";
    return std::string(operation) + ": " + (detail != NULL ? detail : "");
}

/* ---- operation parameter declarations (mirror the generated contract) ---- */

const ParamSpec kFindByIdParams[] = {
    {"id", Lane::Integer}
};
const ParamSpec kFindByEmailParams[] = {
    {"email", Lane::Text}
};
const ParamSpec kListParams[] = {
    {"limit", Lane::Integer},
    {"offset", Lane::Integer}
};
const ParamSpec kInsertParams[] = {
    {"id", Lane::Integer},
    {"name", Lane::Text},
    {"email", Lane::Text},
    {"createdAt", Lane::Integer}
};
const ParamSpec kDeleteByIdParams[] = {
    {"id", Lane::Integer}
};
const ParamSpec kUpdateNameParams[] = {
    {"id", Lane::Integer},
    {"name", Lane::Text}
};

} /* namespace */

/* ---- handshake and lifecycle ---- */

ESABI_DIRECT_FUNCTION(ormPing) {
    (void)argv;
    (void)argc;
    if (retval == NULL) {
        return ESABI_ERR_BAD_ARGUMENTS;
    }
    esabi_value_set_i32(retval, 42);
    return ESABI_OK;
}

ESABI_DIRECT_FUNCTION(ormVersion) {
    (void)argv;
    (void)argc;
    if (retval == NULL) {
        return ESABI_ERR_BAD_ARGUMENTS;
    }
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        return emit_string(retval, esdb_generated_user::expected_ir_hash());
    });
}

ESABI_DIRECT_FUNCTION(ormLastError) {
    (void)argv;
    (void)argc;
    if (retval == NULL) {
        return ESABI_ERR_BAD_ARGUMENTS;
    }
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!g_last_error.set) {
            return emit_string(retval, esdb_orm_user::wire::reply_ok());
        }
        return emit_string(
            retval,
            esdb_orm_user::wire::reply_error(g_last_error.status, g_last_error.message));
    });
}

ESABI_DIRECT_FUNCTION(ormOpen) {
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (retval == NULL) {
            return ESABI_ERR_BAD_ARGUMENTS;
        }
        if (std::strcmp(
                esdb_generated_user::expected_ir_hash(),
                esdb_generated_user_migrations::expected_ir_hash()) != 0) {
            return emit_error_reply(
                retval, ESABI_ERR_INTERNAL,
                "generated repository and migration package IR hashes disagree");
        }
        if (argc != 2) {
            return emit_error_reply(
                retval, ESABI_ERR_BAD_ARGUMENTS, "ormOpen expects (0, pathHex)");
        }
        const char *path_hex = esabi_arg_get_string(argv, argc, 1);
        std::vector<unsigned char> path_bytes;
        if (!esdb_orm_user::wire::hex_decode(path_hex, &path_bytes)) {
            return emit_error_reply(
                retval, ESABI_ERR_BAD_ARGUMENTS, "ormOpen path must be even-length hex");
        }
        const std::string path(path_bytes.begin(), path_bytes.end());
        if (path.empty()) {
            return emit_error_reply(retval, ESABI_ERR_BAD_ARGUMENTS, "ormOpen path is empty");
        }
        if (path.find('\0') != std::string::npos ||
            !esdb_orm_user::wire::utf8_valid(path)) {
            return emit_error_reply(
                retval, ESABI_ERR_BAD_ARGUMENTS, "ormOpen path is not valid UTF-8 text");
        }

        esdb_open_options options;
        esdb_open_options_init(&options);
        options.journal_mode = ESDB_JOURNAL_WAL;
        options.synchronous = ESDB_SYNCHRONOUS_FULL;

        esdb_error error;
        esdb_error_clear(&error);
        esdb_database *database = NULL;
        if (esdb_open(path.c_str(), &options, &database, &error) != ESDB_OK) {
            return emit_error_reply(
                retval, ESABI_ERR_NO_FILE,
                std::string("esdb_open: ") + error.message);
        }

        esdb_error migration_error;
        esdb_error_clear(&migration_error);
        const esdb_status migration_status = esdb_generated_user_migrations::migrate(database, &migration_error);
        if (migration_status != ESDB_OK) {
            const std::string detail = migration_error.message[0] != '\0'
                ? migration_error.message
                : "esdb_migrate failed";
            esdb_close(database);
            return emit_error_reply(
                retval, ESABI_ERR_NO_FILE, std::string("migrate: ") + detail);
        }
        std::uint32_t user_version = 0u;
        esdb_error_clear(&migration_error);
        if (esdb_user_version_get(database, &user_version, &migration_error) != ESDB_OK ||
            user_version != esdb_generated_user_migrations::target_version()) {
            const std::string detail = migration_error.message[0] != '\0'
                ? migration_error.message
                : "user_version does not match generated migration target";
            esdb_close(database);
            return emit_error_reply(
                retval, ESABI_ERR_NO_FILE, std::string("migrate verify: ") + detail);
        }

        esdb_generated_user::UserRepository *repository = NULL;
        try {
            repository = new esdb_generated_user::UserRepository(database);
        } catch (const std::bad_alloc &) {
            esdb_close(database);
            return ESABI_ERR_OUT_OF_MEMORY;
        } catch (...) {
            esdb_close(database);
            return ESABI_ERR_INTERNAL;
        }
        if (!repository->valid()) {
            const std::string message =
                std::string("repository prepare failed: ") + repository->last_error_message();
            delete repository;
            esdb_close(database);
            return emit_error_reply(retval, ESABI_ERR_NO_FILE, message);
        }

        for (std::uint32_t i = 0; i < kSlotCount; i += 1u) {
            Slot &slot = g_slots[i];
            if (slot.in_use || slot.retired) {
                continue;
            }
            if (slot.generation == 0u) {
                slot.generation = 1u;
            }
            slot.database = database;
            slot.repository = repository;
            slot.in_use = true;
            return emit_string(
                retval,
                esdb_orm_user::wire::reply_handle(encode_handle(i, slot.generation)));
        }

        delete repository;
        esdb_close(database);
        return emit_error_reply(
            retval, ESABI_ERR_IO, "bridge handle slots exhausted");
    });
}

ESABI_DIRECT_FUNCTION(ormClose) {
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (retval == NULL) {
            return ESABI_ERR_BAD_ARGUMENTS;
        }
        if (argc != 1) {
            return emit_error_reply(
                retval, ESABI_ERR_BAD_ARGUMENTS, "ormClose expects (handle)");
        }
        esabi_i32 handle = 0;
        if (!esabi_arg_get_i32(argv, argc, 0, &handle)) {
            return emit_error_reply(
                retval, ESABI_ERR_BAD_ARGUMENTS, "ormClose expects an i32 handle");
        }
        Slot *slot = slot_for_handle(handle);
        if (slot == NULL) {
            return emit_error_reply(
                retval, ESABI_ERR_INVALID_OBJECT, "unknown or closed handle");
        }
        release_slot(slot);
        return emit_string(retval, esdb_orm_user::wire::reply_changes(1));
    });
}

/* ---- named operations ---- */

ESABI_DIRECT_FUNCTION(userFindById) {
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        std::int32_t handle = 0;
        std::vector<ParsedValue> values;
        std::string message;
        if (!parse_arguments(argv, argc, kFindByIdParams, 1u, &handle, &values, &message)) {
            return emit_error_reply(retval, ESABI_ERR_BAD_ARGUMENTS, message);
        }
        Slot *slot = slot_for_handle(handle);
        if (slot == NULL) {
            return emit_error_reply(retval, ESABI_ERR_INVALID_OBJECT, "unknown or closed handle");
        }
        const ParsedValue *id = find_value(values, kFindByIdParams, "id");
        esdb_generated_user::UserRow row;
        const int result = slot->repository->find_by_id(id->integer, &row);
        if (result < 0) {
            return emit_error_reply(retval, ESABI_ERR_IO, repository_error(*slot, "find_by_id"));
        }
        if (result == 0) {
            return emit_string(retval, esdb_orm_user::wire::reply_none());
        }
        return emit_string(retval, row_wire(row));
    });
}

ESABI_DIRECT_FUNCTION(userFindByEmail) {
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        std::int32_t handle = 0;
        std::vector<ParsedValue> values;
        std::string message;
        if (!parse_arguments(argv, argc, kFindByEmailParams, 1u, &handle, &values, &message)) {
            return emit_error_reply(retval, ESABI_ERR_BAD_ARGUMENTS, message);
        }
        Slot *slot = slot_for_handle(handle);
        if (slot == NULL) {
            return emit_error_reply(retval, ESABI_ERR_INVALID_OBJECT, "unknown or closed handle");
        }
        const ParsedValue *email = find_value(values, kFindByEmailParams, "email");
        esdb_generated_user::UserRow row;
        const int result = slot->repository->find_by_email(email->text, &row);
        if (result < 0) {
            return emit_error_reply(
                retval, ESABI_ERR_IO, repository_error(*slot, "find_by_email"));
        }
        if (result == 0) {
            return emit_string(retval, esdb_orm_user::wire::reply_none());
        }
        return emit_string(retval, row_wire(row));
    });
}

ESABI_DIRECT_FUNCTION(userList) {
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        std::int32_t handle = 0;
        std::vector<ParsedValue> values;
        std::string message;
        if (!parse_arguments(argv, argc, kListParams, 2u, &handle, &values, &message)) {
            return emit_error_reply(retval, ESABI_ERR_BAD_ARGUMENTS, message);
        }
        Slot *slot = slot_for_handle(handle);
        if (slot == NULL) {
            return emit_error_reply(retval, ESABI_ERR_INVALID_OBJECT, "unknown or closed handle");
        }
        const ParsedValue *limit = find_value(values, kListParams, "limit");
        const ParsedValue *offset = find_value(values, kListParams, "offset");
        std::vector<esdb_generated_user::UserRow> rows;
        const int result = slot->repository->list(limit->integer, offset->integer, &rows);
        if (result < 0) {
            return emit_error_reply(retval, ESABI_ERR_IO, repository_error(*slot, "list"));
        }
        return emit_string(retval, rows_wire(rows));
    });
}

ESABI_DIRECT_FUNCTION(userInsert) {
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        std::int32_t handle = 0;
        std::vector<ParsedValue> values;
        std::string message;
        if (!parse_arguments(argv, argc, kInsertParams, 4u, &handle, &values, &message)) {
            return emit_error_reply(retval, ESABI_ERR_BAD_ARGUMENTS, message);
        }
        Slot *slot = slot_for_handle(handle);
        if (slot == NULL) {
            return emit_error_reply(retval, ESABI_ERR_INVALID_OBJECT, "unknown or closed handle");
        }
        const ParsedValue *id = find_value(values, kInsertParams, "id");
        const ParsedValue *name = find_value(values, kInsertParams, "name");
        const ParsedValue *email = find_value(values, kInsertParams, "email");
        const ParsedValue *created_at = find_value(values, kInsertParams, "createdAt");
        esdb_generated_user::UserInsert row;
        row.id = id->integer;
        row.name = name->text;
        row.email = email->text;
        row.created_at = created_at->integer;
        const int result = slot->repository->insert(row);
        if (result < 0) {
            return emit_error_reply(retval, ESABI_ERR_IO, repository_error(*slot, "insert"));
        }
        return emit_string(retval, esdb_orm_user::wire::reply_changes(result));
    });
}

ESABI_DIRECT_FUNCTION(userDeleteById) {
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        std::int32_t handle = 0;
        std::vector<ParsedValue> values;
        std::string message;
        if (!parse_arguments(argv, argc, kDeleteByIdParams, 1u, &handle, &values, &message)) {
            return emit_error_reply(retval, ESABI_ERR_BAD_ARGUMENTS, message);
        }
        Slot *slot = slot_for_handle(handle);
        if (slot == NULL) {
            return emit_error_reply(retval, ESABI_ERR_INVALID_OBJECT, "unknown or closed handle");
        }
        const ParsedValue *id = find_value(values, kDeleteByIdParams, "id");
        const int result = slot->repository->delete_by_id(id->integer);
        if (result < 0) {
            return emit_error_reply(
                retval, ESABI_ERR_IO, repository_error(*slot, "delete_by_id"));
        }
        return emit_string(retval, esdb_orm_user::wire::reply_changes(result));
    });
}

ESABI_DIRECT_FUNCTION(userUpdateName) {
    return guarded([&]() -> esabi_error {
        std::lock_guard<std::mutex> guard(g_mutex);
        std::int32_t handle = 0;
        std::vector<ParsedValue> values;
        std::string message;
        if (!parse_arguments(argv, argc, kUpdateNameParams, 2u, &handle, &values, &message)) {
            return emit_error_reply(retval, ESABI_ERR_BAD_ARGUMENTS, message);
        }
        Slot *slot = slot_for_handle(handle);
        if (slot == NULL) {
            return emit_error_reply(retval, ESABI_ERR_INVALID_OBJECT, "unknown or closed handle");
        }
        /*
         * Semantic mapping: SQL binds the SET value before the WHERE key while
         * the repository API takes (id, name). Resolve by generated parameter
         * name so UPDATE stays correct if that bind order ever differs.
         */
        const ParsedValue *id = find_value(values, kUpdateNameParams, "id");
        const ParsedValue *name = find_value(values, kUpdateNameParams, "name");
        const int result = slot->repository->update_name(id->integer, name->text);
        if (result < 0) {
            return emit_error_reply(
                retval, ESABI_ERR_IO, repository_error(*slot, "update_name"));
        }
        return emit_string(retval, esdb_orm_user::wire::reply_changes(result));
    });
}

/* ---- ESABI lifecycle entry points ---- */

static char g_signatures[] =
    ESABI_SIGNATURE(ormPing, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(ormVersion, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(ormOpen, ESABI_SIG_DOUBLE ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(ormClose, ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(ormLastError, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(userFindById, ESABI_SIG_I32 ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(userFindByEmail, ESABI_SIG_I32 ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(userList, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(userInsert, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(userDeleteById, ESABI_SIG_I32 ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(userUpdateName, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING)
    ;

ESABI_INITIALIZE_FUNCTION {
    (void)argv;
    (void)argc;
    return g_signatures;
}

ESABI_VERSION_FUNCTION {
    /* Generated contract revision, matching the reference stub. */
    return 1;
}

ESABI_FREE_FUNCTION {
    std::free(pointer);
}

ESABI_TERMINATE_FUNCTION {
    try {
        std::lock_guard<std::mutex> guard(g_mutex);
        for (std::uint32_t i = 0; i < kSlotCount; i += 1u) {
            if (g_slots[i].in_use) {
                release_slot(&g_slots[i]);
            }
        }
        g_last_error.set = false;
        g_last_error.status = 0;
        g_last_error.message.clear();
    } catch (...) {
        /* ESTerminate must not throw across the ESABI boundary. */
    }
}
