#include "esdb_externalobject_abi.h"

#include <esdb/esdb.h>

#include "esdb_nothrow_mutex.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>

namespace {

constexpr std::uint32_t k_slot_count = 255u;
constexpr std::uint32_t k_slot_bits = 8u;
constexpr std::uint32_t k_generation_max =
    static_cast<std::uint32_t>(INT32_MAX) >> k_slot_bits;

struct Slot {
    esdb_database *database = nullptr;
    std::uint32_t generation = 1u;
    bool retired = false;
};

struct AdapterError {
    std::uint32_t code = 0u;
    esdb_error database_error{};
    char message[ESDB_ERROR_MESSAGE_CAPACITY]{};
};

std::array<Slot, k_slot_count> g_slots{};
esdb_detail::NoThrowMutex g_mutex;
thread_local std::string g_staged_path;
thread_local AdapterError g_last_error{};

enum : std::uint32_t {
    ADAPTER_OK = 0u,
    ADAPTER_BAD_ARGUMENT = 1u,
    ADAPTER_NO_STAGED_PATH = 2u,
    ADAPTER_OPEN_FAILED = 3u,
    ADAPTER_HANDLE_EXHAUSTED = 4u,
    ADAPTER_INVALID_HANDLE = 5u,
    ADAPTER_DATABASE_ERROR = 6u,
    ADAPTER_OUT_OF_MEMORY = 7u,
    ADAPTER_INTERNAL = 8u
};

void copy_text(char *dest, std::size_t capacity, const char *text) noexcept {
    if (!dest || capacity == 0u) return;
    if (!text) {
        dest[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(dest, capacity, text, _TRUNCATE);
#else
    std::strncpy(dest, text, capacity - 1u);
    dest[capacity - 1u] = '\0';
#endif
}

void clear_last_error_unlocked() noexcept {
    g_last_error = AdapterError{};
}

void set_last_error_unlocked(
    std::uint32_t code,
    const char *message,
    const esdb_error *database_error = nullptr) noexcept {
    g_last_error = AdapterError{};
    g_last_error.code = code;
    if (database_error) g_last_error.database_error = *database_error;
    copy_text(g_last_error.message, sizeof(g_last_error.message), message);
}

void set_last_error(
    std::uint32_t code,
    const char *message,
    const esdb_error *database_error = nullptr) noexcept {
    set_last_error_unlocked(code, message, database_error);
}

void clear_last_error() noexcept {
    clear_last_error_unlocked();
}

bool get_dummy(esabi_value *argv, esabi_long argc) noexcept {
    double ignored = 0.0;
    return argc == 1 && esabi_arg_get_double(argv, argc, 0, &ignored) != 0;
}

bool get_handle(esabi_value *argv, esabi_long argc, std::int32_t &out) noexcept {
    esabi_i32 value = 0;
    if (argc != 1 || !esabi_arg_get_i32(argv, argc, 0, &value)) return false;
    if (value <= 0) return false;
    out = value;
    return true;
}

std::int32_t encode_handle(std::uint32_t slot_index, std::uint32_t generation) noexcept {
    const std::uint32_t token =
        (generation << k_slot_bits) | (slot_index + 1u);
    return static_cast<std::int32_t>(token);
}

bool decode_handle(
    std::int32_t token,
    std::uint32_t &slot_index,
    std::uint32_t &generation) noexcept {
    if (token <= 0) return false;
    const std::uint32_t raw = static_cast<std::uint32_t>(token);
    const std::uint32_t encoded_slot = raw & 0xffu;
    if (encoded_slot == 0u || encoded_slot > k_slot_count) return false;
    generation = raw >> k_slot_bits;
    if (generation == 0u || generation > k_generation_max) return false;
    slot_index = encoded_slot - 1u;
    return true;
}

Slot *lookup_slot_unlocked(std::int32_t token) noexcept {
    std::uint32_t slot_index = 0;
    std::uint32_t generation = 0;
    if (!decode_handle(token, slot_index, generation)) return nullptr;
    Slot &slot = g_slots[slot_index];
    if (slot.retired || !slot.database || slot.generation != generation) return nullptr;
    return &slot;
}

void advance_generation(Slot &slot) noexcept {
    if (slot.generation >= k_generation_max) {
        // Never wrap a generation: wrapping could make an ancient stale token
        // valid again (ABA). Retiring one slot after ~8.4M reuses is safer.
        slot.retired = true;
        return;
    }
    ++slot.generation;
}

char *duplicate_string(const std::string &text) noexcept {
    if (text.size() >= std::numeric_limits<std::size_t>::max() - 1u) return nullptr;
    char *copy = static_cast<char *>(std::malloc(text.size() + 1u));
    if (!copy) return nullptr;
    std::memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    return copy;
}

bool set_return_string(esabi_value *retval, const std::string &text) noexcept {
    char *copy = duplicate_string(text);
    if (!copy) {
        esabi_value_set_undefined(retval);
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to allocate returned string");
        return false;
    }
    esabi_value_set_string(retval, copy);
    return true;
}

std::string json_escape(const char *text) {
    std::string out;
    if (!text) return out;
    while (*text) {
        const unsigned char c = static_cast<unsigned char>(*text++);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char escaped[7]{};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

const char *journal_name(esdb_journal_mode mode) noexcept {
    switch (mode) {
        case ESDB_JOURNAL_DELETE: return "delete";
        case ESDB_JOURNAL_TRUNCATE: return "truncate";
        case ESDB_JOURNAL_PERSIST: return "persist";
        case ESDB_JOURNAL_MEMORY: return "memory";
        case ESDB_JOURNAL_WAL: return "wal";
        case ESDB_JOURNAL_OFF: return "off";
        default: return "unchanged";
    }
}

const char *synchronous_name(esdb_synchronous_mode mode) noexcept {
    switch (mode) {
        case ESDB_SYNCHRONOUS_OFF: return "off";
        case ESDB_SYNCHRONOUS_NORMAL: return "normal";
        case ESDB_SYNCHRONOUS_FULL: return "full";
        case ESDB_SYNCHRONOUS_EXTRA: return "extra";
        default: return "unchanged";
    }
}

std::string error_json_unlocked() {
    std::ostringstream out;
    out << "{\"ok\":" << (g_last_error.code == ADAPTER_OK ? "true" : "false")
        << ",\"adapterCode\":" << g_last_error.code
        << ",\"message\":\"" << json_escape(g_last_error.message) << "\""
        << ",\"status\":" << g_last_error.database_error.status
        << ",\"statusName\":\""
        << json_escape(esdb_status_name(g_last_error.database_error.status))
        << "\""
        << ",\"phase\":" << g_last_error.database_error.phase
        << ",\"phaseName\":\""
        << json_escape(esdb_phase_name(g_last_error.database_error.phase))
        << "\""
        << ",\"sqliteCode\":" << g_last_error.database_error.sqlite_code
        << ",\"sqliteExtendedCode\":"
        << g_last_error.database_error.sqlite_extended_code
        << ",\"databaseMessage\":\""
        << json_escape(g_last_error.database_error.message)
        << "\"}";
    return out.str();
}

std::string health_json(const esdb_database_health &health) {
    std::ostringstream out;
    out << "{\"ok\":true"
        << ",\"pageSize\":" << health.page_size
        << ",\"pageCount\":\"" << health.page_count << "\""
        << ",\"freelistCount\":\"" << health.freelist_count << "\""
        << ",\"databaseBytes\":\"" << health.database_bytes << "\""
        << ",\"configuredCacheKiB\":\"" << health.configured_cache_kib << "\""
        << ",\"journalMode\":\"" << journal_name(health.journal_mode) << "\""
        << ",\"synchronous\":\"" << synchronous_name(health.synchronous) << "\""
        << ",\"busyTimeoutMs\":" << health.busy_timeout_ms
        << ",\"userVersion\":" << health.user_version
        << ",\"inTransaction\":" << (health.in_transaction ? "true" : "false")
        << ",\"savepointDepth\":" << health.savepoint_depth
        << ",\"operationCount\":\"" << health.operation_count << "\""
        << ",\"errorCount\":\"" << health.error_count << "\""
        << ",\"busyCount\":\"" << health.busy_count << "\""
        << ",\"commitCount\":\"" << health.commit_count << "\""
        << ",\"rollbackCount\":\"" << health.rollback_count << "\""
        << ",\"lastErrorStatus\":" << health.last_error_status
        << ",\"lastErrorPhase\":" << health.last_error_phase
        << ",\"lastErrorSqliteCode\":" << health.last_error_sqlite_code
        << ",\"lastErrorSqliteExtendedCode\":"
        << health.last_error_sqlite_extended_code
        << "}";
    return out.str();
}

static char g_signatures[] =
    ESABI_SIGNATURE(ping, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(abiVersion, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(version, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(sqliteVersion, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(stage, ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(openStaged, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(close, ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(health, ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(lastError, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(dataVersion, ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(handleCount, ESABI_SIG_DOUBLE);

}  // namespace

ESABI_INITIALIZE_FUNCTION {
    (void)argv;
    (void)argc;
    return g_signatures;
}

ESABI_VERSION_FUNCTION {
    return 1;
}

ESABI_FREE_FUNCTION {
    std::free(pointer);
}

ESABI_TERMINATE_FUNCTION {
    try {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(g_mutex);
        for (Slot &slot : g_slots) {
            if (slot.database) {
                esdb_close(slot.database);
                slot.database = nullptr;
                advance_generation(slot);
            }
        }
        g_staged_path.clear();
        clear_last_error_unlocked();
    } catch (...) {
        /* Host shutdown boundary: never allow a C++ exception to escape. */
    }
}

ESABI_DIRECT_FUNCTION(ping) {
    try {
        if (!get_dummy(argv, argc)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "ping expects one dummy double argument");
            esabi_value_set_i32(retval, 0);
            return ESABI_OK;
        }
        clear_last_error();
        esabi_value_set_i32(retval, 42);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in ping");
        esabi_value_set_i32(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(abiVersion) {
    try {
        if (!get_dummy(argv, argc)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "abiVersion expects one dummy double argument");
            esabi_value_set_i32(retval, 0);
            return ESABI_OK;
        }
        clear_last_error();
        esabi_value_set_i32(retval, static_cast<esabi_i32>(esdb_abi_version()));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in abiVersion");
        esabi_value_set_i32(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(version) {
    try {
        if (!get_dummy(argv, argc)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "version expects one dummy double argument");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(retval, esdb_version());
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in version");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(sqliteVersion) {
    try {
        if (!get_dummy(argv, argc)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "sqliteVersion expects one dummy double argument");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(retval, esdb_sqlite_version());
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in sqliteVersion");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(stage) {
    try {
        const char *path = argc == 1 ? esabi_arg_get_string(argv, argc, 0) : nullptr;
        if (!path || !path[0]) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "stage expects one non-empty string path");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        g_staged_path.assign(path);
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, 1);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to stage database path");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in stage");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(openStaged) {
    try {
        if (!get_dummy(argv, argc)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "openStaged expects one dummy double argument");
            esabi_value_set_i32(retval, 0);
            return ESABI_OK;
        }

        if (g_staged_path.empty()) {
            set_last_error_unlocked(ADAPTER_NO_STAGED_PATH, "no database path has been staged");
            esabi_value_set_i32(retval, 0);
            return ESABI_OK;
        }
        const std::string path = g_staged_path;

        esdb_open_options options{};
        esdb_open_options_init(&options);
        esdb_database *database = nullptr;
        esdb_error error{};
        const esdb_status status =
            esdb_open(path.c_str(), &options, &database, &error);
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_OPEN_FAILED, "ESDB failed to open the staged database", &error);
            esabi_value_set_i32(retval, 0);
            return ESABI_OK;
        }

        std::lock_guard<esdb_detail::NoThrowMutex> lock(g_mutex);
        for (std::uint32_t index = 0; index < k_slot_count; ++index) {
            Slot &slot = g_slots[index];
            if (!slot.database && !slot.retired) {
                slot.database = database;
                clear_last_error_unlocked();
                esabi_value_set_i32(retval, encode_handle(index, slot.generation));
                return ESABI_OK;
            }
        }

        esdb_close(database);
        set_last_error_unlocked(
            ADAPTER_HANDLE_EXHAUSTED,
            "all ExternalObject database handle slots are in use");
        esabi_value_set_i32(retval, 0);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to allocate while opening database");
        esabi_value_set_i32(retval, 0);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in openStaged");
        esabi_value_set_i32(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(close) {
    try {
        std::int32_t handle = 0;
        if (!get_handle(argv, argc, handle)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "close expects one positive integer handle");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        std::lock_guard<esdb_detail::NoThrowMutex> lock(g_mutex);
        Slot *slot = lookup_slot_unlocked(handle);
        if (!slot) {
            set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        esdb_close(slot->database);
        slot->database = nullptr;
        advance_generation(*slot);
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, 1);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in close");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(health) {
    try {
        std::int32_t handle = 0;
        if (!get_handle(argv, argc, handle)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "health expects one positive integer handle");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        esdb_database_health health_snapshot{};
        health_snapshot.struct_size = sizeof(health_snapshot);
        {
            std::lock_guard<esdb_detail::NoThrowMutex> lock(g_mutex);
            Slot *slot = lookup_slot_unlocked(handle);
            if (!slot) {
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            esdb_error error{};
            const esdb_status status =
                esdb_database_health_get(slot->database, &health_snapshot, &error);
            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR,
                    "ESDB failed to read database health",
                    &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }
        set_return_string(retval, health_json(health_snapshot));
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to serialize health result");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in health");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(lastError) {
    try {
        if (!get_dummy(argv, argc)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "lastError expects one dummy double argument");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        const std::string serialized = error_json_unlocked();
        set_return_string(retval, serialized);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to serialize last error");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in lastError");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(dataVersion) {
    try {
        std::int32_t handle = 0;
        if (!get_handle(argv, argc, handle)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "dataVersion expects one positive integer handle");
            esabi_value_set_double(retval, -1.0);
            return ESABI_OK;
        }
        std::uint32_t version_value = 0;
        {
            std::lock_guard<esdb_detail::NoThrowMutex> lock(g_mutex);
            Slot *slot = lookup_slot_unlocked(handle);
            if (!slot) {
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_double(retval, -1.0);
                return ESABI_OK;
            }
            esdb_error error{};
            const esdb_status status =
                esdb_data_version_get(slot->database, &version_value, &error);
            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR,
                    "ESDB failed to read data_version",
                    &error);
                esabi_value_set_double(retval, -1.0);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }
        esabi_value_set_double(retval, static_cast<double>(version_value));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in dataVersion");
        esabi_value_set_double(retval, -1.0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(handleCount) {
    try {
        if (!get_dummy(argv, argc)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "handleCount expects one dummy double argument");
            esabi_value_set_i32(retval, 0);
            return ESABI_OK;
        }
        std::uint32_t count = 0;
        {
            std::lock_guard<esdb_detail::NoThrowMutex> lock(g_mutex);
            for (const Slot &slot : g_slots) {
                if (slot.database) ++count;
            }
            clear_last_error_unlocked();
        }
        esabi_value_set_i32(retval, static_cast<esabi_i32>(count));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in handleCount");
        esabi_value_set_i32(retval, 0);
        return ESABI_OK;
    }
}
