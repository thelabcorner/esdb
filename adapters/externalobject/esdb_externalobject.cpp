#include "esdb_externalobject_abi.h"

#include <esdb/esdb.h>
#include <esdb/esdb_store.h>
#include <esdb/esdb_object_store.h>
#include <sqlite3.h>

#include "esdb_nothrow_mutex.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t k_slot_count = 255u;
constexpr std::uint32_t k_slot_bits = 8u;
constexpr std::uint32_t k_generation_max =
    static_cast<std::uint32_t>(INT32_MAX) >> k_slot_bits;

struct Slot {
    esdb_database *database = nullptr;
    esdb_transaction *transaction = nullptr;
    esdb_detail::NoThrowMutex operation_mutex;
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

Slot *lock_slot(
    std::int32_t token,
    std::unique_lock<esdb_detail::NoThrowMutex> &operation_lock) noexcept {
    std::unique_lock<esdb_detail::NoThrowMutex> table_lock(g_mutex);
    Slot *slot = lookup_slot_unlocked(token);
    if (!slot) return nullptr;

    /*
     * Acquire the per-slot lock while the table lock still guarantees that the
     * slot cannot be closed/reused. DB work continues after releasing the
     * table lock, so a busy database never serializes unrelated handles.
     */
    operation_lock =
        std::unique_lock<esdb_detail::NoThrowMutex>(slot->operation_mutex);
    table_lock.unlock();
    return slot;
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

bool get_i32_arg(
    esabi_value *argv,
    esabi_long argc,
    esabi_long index,
    std::int32_t &out) noexcept {
    esabi_i32 value = 0;
    if (!esabi_arg_get_i32(argv, argc, index, &value)) return false;
    out = value;
    return true;
}

const char *get_string_arg(
    esabi_value *argv,
    esabi_long argc,
    esabi_long index) noexcept {
    return esabi_arg_get_string(argv, argc, index);
}

bool get_double_arg(
    esabi_value *argv,
    esabi_long argc,
    esabi_long index,
    double &out) noexcept {
    return esabi_arg_get_double(argv, argc, index, &out) != 0;
}

bool canonical_decimal_syntax(
    const char *text,
    bool allow_negative) noexcept {
    if (!text || !text[0]) return false;
    const char *cursor = text;
    if (*cursor == '-') {
        if (!allow_negative) return false;
        ++cursor;
        if (!*cursor) return false;
    }
    if (*cursor == '0') {
        return cursor[1] == '\0';
    }
    if (*cursor < '1' || *cursor > '9') return false;
    ++cursor;
    while (*cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        ++cursor;
    }
    return true;
}

bool parse_i64_decimal(const char *text, std::int64_t &out) noexcept {
    if (!canonical_decimal_syntax(text, true)) return false;
    if (std::strcmp(text, "-0") == 0) return false;
    errno = 0;
    char *end = nullptr;
#if defined(_MSC_VER)
    const __int64 value = _strtoi64(text, &end, 10);
#else
    const long long value = std::strtoll(text, &end, 10);
#endif
    if (errno == ERANGE || !end || *end != '\0') return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

bool parse_revision_decimal(const char *text, std::uint64_t &out) noexcept {
    if (!canonical_decimal_syntax(text, false)) return false;
    errno = 0;
    char *end = nullptr;
#if defined(_MSC_VER)
    const unsigned __int64 value = _strtoui64(text, &end, 10);
#else
    const unsigned long long value = std::strtoull(text, &end, 10);
#endif
    if (errno == ERANGE || !end || *end != '\0' ||
        value > static_cast<unsigned long long>(ESDB_REVISION_MAX)) {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

std::string u64_decimal(std::uint64_t value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
}

std::string i64_decimal(std::int64_t value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
}

std::string double_decimal(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return std::signbit(value) ? "-Infinity" : "Infinity";
    if (value == 0.0 && std::signbit(value)) return "-0";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

bool parse_double_decimal(const char *text, double &out) noexcept {
    if (!text || !text[0]) return false;
    if (std::strcmp(text, "NaN") == 0) {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (std::strcmp(text, "Infinity") == 0) {
        out = std::numeric_limits<double>::infinity();
        return true;
    }
    if (std::strcmp(text, "-Infinity") == 0) {
        out = -std::numeric_limits<double>::infinity();
        return true;
    }
    if (std::strcmp(text, "-0") == 0) {
        out = -0.0;
        return true;
    }

    errno = 0;
    char *end = nullptr;
    const double value = std::strtod(text, &end);
    if (errno == ERANGE || !end || *end != '\0') return false;
    out = value;
    return true;
}

int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool hex_decode(const char *text, std::vector<std::uint8_t> &out) {
    if (!text) return false;
    const std::size_t length = std::strlen(text);
    if ((length & 1u) != 0u) return false;
    out.clear();
    out.reserve(length / 2u);
    for (std::size_t index = 0; index < length; index += 2u) {
        const int hi = hex_nibble(text[index]);
        const int lo = hex_nibble(text[index + 1u]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool decode_hex_cstring(
    const char *text,
    std::string &out,
    bool allow_empty = false) {
    std::vector<std::uint8_t> decoded;
    if (!hex_decode(text, decoded)) return false;
    if (decoded.empty() && !allow_empty) return false;
    for (std::uint8_t byte : decoded) {
        if (byte == 0u) return false;
    }
    out.assign(
        reinterpret_cast<const char *>(decoded.data()),
        decoded.size());
    return true;
}

bool decode_store_key_args(
    esabi_value *argv,
    esabi_long argc,
    esabi_long store_index,
    esabi_long key_index,
    std::string &store_name,
    std::string &key) {
    const char *store_hex = get_string_arg(argv, argc, store_index);
    const char *key_hex = get_string_arg(argv, argc, key_index);
    return store_hex && key_hex &&
           decode_hex_cstring(store_hex, store_name, false) &&
           decode_hex_cstring(key_hex, key, false);
}

std::string hex_encode(const void *data, std::size_t size) {
    static const char digits[] = "0123456789ABCDEF";
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::string out;
    out.resize(size * 2u);
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t value = bytes[index];
        out[index * 2u] = digits[value >> 4u];
        out[index * 2u + 1u] = digits[value & 0x0fu];
    }
    return out;
}

esdb_status make_number_value(
    std::int32_t type,
    double number,
    esdb_value **out_value,
    esdb_error *error) noexcept {
    switch (static_cast<esdb_value_type>(type)) {
        case ESDB_VALUE_BOOL:
            if (number != 0.0 && number != 1.0) return ESDB_ERR_INVALID_ARGUMENT;
            return esdb_value_create_bool(number != 0.0, out_value, error);
        case ESDB_VALUE_INT32:
            if (!std::isfinite(number) || std::floor(number) != number ||
                number < static_cast<double>(INT32_MIN) ||
                number > static_cast<double>(INT32_MAX)) {
                return ESDB_ERR_INVALID_ARGUMENT;
            }
            return esdb_value_create_int32(
                static_cast<std::int32_t>(number), out_value, error);
        case ESDB_VALUE_DOUBLE:
            return esdb_value_create_double(number, out_value, error);
        default:
            return ESDB_ERR_INVALID_ARGUMENT;
    }
}

esdb_status make_text_value(
    std::int32_t type,
    const char *payload,
    esdb_value **out_value,
    esdb_error *error) {
    const auto value_type = static_cast<esdb_value_type>(type);
    if (value_type == ESDB_VALUE_NULL) {
        if (!payload || payload[0] != '\0') return ESDB_ERR_INVALID_ARGUMENT;
        return esdb_value_create_null(out_value, error);
    }
    if (value_type == ESDB_VALUE_INT64) {
        std::int64_t value = 0;
        if (!parse_i64_decimal(payload, value)) return ESDB_ERR_INVALID_ARGUMENT;
        return esdb_value_create_int64(value, out_value, error);
    }

    std::vector<std::uint8_t> decoded;
    if (!hex_decode(payload, decoded)) return ESDB_ERR_INVALID_ARGUMENT;
    const void *bytes = decoded.empty() ? nullptr : decoded.data();
    const std::uint64_t size = static_cast<std::uint64_t>(decoded.size());

    if (value_type == ESDB_VALUE_BYTES) {
        return esdb_value_create_bytes(bytes, size, out_value, error);
    }
    if (value_type == ESDB_VALUE_UTF8 ||
        value_type == ESDB_VALUE_ARRAY ||
        value_type == ESDB_VALUE_OBJECT) {
        return esdb_value_create_text(
            value_type,
            decoded.empty() ? nullptr : reinterpret_cast<const char *>(decoded.data()),
            size,
            out_value,
            error);
    }
    return ESDB_ERR_INVALID_ARGUMENT;
}

bool value_wire(const esdb_value *value, std::string &out) {
    if (!value) return false;
    const esdb_value_type type = esdb_value_type_of(value);
    out = "V1:" + std::to_string(static_cast<unsigned>(type)) + ":";

    switch (type) {
        case ESDB_VALUE_NULL:
            return true;
        case ESDB_VALUE_BOOL: {
            int v = 0;
            if (esdb_value_get_bool(value, &v) != ESDB_OK) return false;
            out += v ? "1" : "0";
            return true;
        }
        case ESDB_VALUE_INT32: {
            std::int32_t v = 0;
            if (esdb_value_get_int32(value, &v) != ESDB_OK) return false;
            out += std::to_string(v);
            return true;
        }
        case ESDB_VALUE_INT64: {
            std::int64_t v = 0;
            if (esdb_value_get_int64(value, &v) != ESDB_OK) return false;
            out += i64_decimal(v);
            return true;
        }
        case ESDB_VALUE_DOUBLE: {
            double v = 0.0;
            if (esdb_value_get_double(value, &v) != ESDB_OK) return false;
            out += double_decimal(v);
            return true;
        }
        case ESDB_VALUE_UTF8:
        case ESDB_VALUE_BYTES:
        case ESDB_VALUE_ARRAY:
        case ESDB_VALUE_OBJECT: {
            const void *data = nullptr;
            std::uint64_t size = 0u;
            if (esdb_value_get_data(value, &data, &size) != ESDB_OK) return false;
            if (size > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max() / 2u)) {
                return false;
            }
            out += hex_encode(data, static_cast<std::size_t>(size));
            return true;
        }
        default:
            return false;
    }
}


constexpr std::uint32_t k_query_parameter_max = 1024u;
constexpr std::uint32_t k_query_row_max = 10000u;
constexpr std::size_t k_query_sql_max_bytes = 1024u * 1024u;
constexpr std::size_t k_query_parameter_wire_max_bytes = 8u * 1024u * 1024u;
constexpr std::size_t k_query_result_wire_max_bytes = 16u * 1024u * 1024u;
constexpr std::size_t k_query_result_header_max_bytes = 128u;

struct QueryParameterGuard {
    std::vector<esdb_value *> values;

    ~QueryParameterGuard() noexcept {
        for (esdb_value *value : values) esdb_value_destroy(value);
    }

    QueryParameterGuard() = default;
    QueryParameterGuard(const QueryParameterGuard &) = delete;
    QueryParameterGuard &operator=(const QueryParameterGuard &) = delete;
};

bool parse_u32_decimal(const std::string &text, std::uint32_t &out) noexcept {
    if (!canonical_decimal_syntax(text.c_str(), false)) return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0' ||
        value > static_cast<unsigned long>(UINT32_MAX)) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool parse_query_value_wire(
    const std::string &wire,
    esdb_value **out_value) {
    if (!out_value || wire.size() < 5u || wire.compare(0u, 3u, "V1:") != 0) {
        return false;
    }
    *out_value = nullptr;
    const std::size_t separator = wire.find(':', 3u);
    if (separator == std::string::npos) return false;

    std::uint32_t type = 0u;
    if (!parse_u32_decimal(wire.substr(3u, separator - 3u), type) ||
        type > static_cast<std::uint32_t>(ESDB_VALUE_OBJECT)) {
        return false;
    }
    const std::string payload = wire.substr(separator + 1u);
    esdb_error error{};

    switch (static_cast<esdb_value_type>(type)) {
        case ESDB_VALUE_NULL:
            return payload.empty() &&
                   esdb_value_create_null(out_value, &error) == ESDB_OK;
        case ESDB_VALUE_BOOL:
            if (payload != "0" && payload != "1") return false;
            return esdb_value_create_bool(
                payload == "1" ? 1 : 0, out_value, &error) == ESDB_OK;
        case ESDB_VALUE_INT32: {
            std::int64_t value = 0;
            if (!parse_i64_decimal(payload.c_str(), value) ||
                value < static_cast<std::int64_t>(INT32_MIN) ||
                value > static_cast<std::int64_t>(INT32_MAX)) {
                return false;
            }
            return esdb_value_create_int32(
                static_cast<std::int32_t>(value), out_value, &error) == ESDB_OK;
        }
        case ESDB_VALUE_INT64: {
            std::int64_t value = 0;
            if (!parse_i64_decimal(payload.c_str(), value)) return false;
            return esdb_value_create_int64(value, out_value, &error) == ESDB_OK;
        }
        case ESDB_VALUE_DOUBLE: {
            double value = 0.0;
            if (!parse_double_decimal(payload.c_str(), value)) return false;
            return esdb_value_create_double(value, out_value, &error) == ESDB_OK;
        }
        case ESDB_VALUE_UTF8:
        case ESDB_VALUE_BYTES:
        case ESDB_VALUE_ARRAY:
        case ESDB_VALUE_OBJECT:
            return make_text_value(
                static_cast<std::int32_t>(type),
                payload.c_str(),
                out_value,
                &error) == ESDB_OK;
        default:
            return false;
    }
}

bool parse_query_parameter_wire(
    const char *text,
    QueryParameterGuard &out) {
    if (!text) return false;
    const std::size_t size = std::strlen(text);
    if (size > k_query_parameter_wire_max_bytes) return false;

    const std::string wire(text, size);
    const std::size_t header_end = wire.find('\n');
    const std::string header =
        header_end == std::string::npos ? wire : wire.substr(0u, header_end);
    if (header.size() < 4u || header.compare(0u, 3u, "P1:") != 0) return false;

    std::uint32_t count = 0u;
    if (!parse_u32_decimal(header.substr(3u), count) ||
        count > k_query_parameter_max) {
        return false;
    }

    out.values.reserve(count);
    std::size_t cursor =
        header_end == std::string::npos ? wire.size() : header_end + 1u;
    for (std::uint32_t index = 0u; index < count; ++index) {
        if (cursor > wire.size()) return false;
        const std::size_t line_end = wire.find('\n', cursor);
        const std::size_t end =
            line_end == std::string::npos ? wire.size() : line_end;
        if (end == cursor) return false;

        esdb_value *value = nullptr;
        if (!parse_query_value_wire(wire.substr(cursor, end - cursor), &value) ||
            !value) {
            esdb_value_destroy(value);
            return false;
        }
        try {
            out.values.push_back(value);
        } catch (...) {
            esdb_value_destroy(value);
            throw;
        }
        cursor = line_end == std::string::npos ? wire.size() : line_end + 1u;
    }

    while (cursor < wire.size() && wire[cursor] == '\n') ++cursor;
    return cursor == wire.size();
}

struct QueryWireContext {
    std::string names;
    std::string rows;
    std::uint32_t column_count = 0u;
    std::uint32_t returned_rows = 0u;
    std::uint32_t max_rows = 0u;
    bool names_ready = false;
    bool truncated = false;
    bool failed = false;
};

struct QueryMetadataStatementGuard {
    sqlite3_stmt *statement = nullptr;

    QueryMetadataStatementGuard() = default;
    QueryMetadataStatementGuard(const QueryMetadataStatementGuard &) = delete;
    QueryMetadataStatementGuard &operator=(const QueryMetadataStatementGuard &) = delete;

    ~QueryMetadataStatementGuard() noexcept {
        finalize();
    }

    void finalize() noexcept {
        if (statement) {
            sqlite3_finalize(statement);
            statement = nullptr;
        }
    }
};

bool set_query_column_names(
    QueryWireContext *context,
    const char *const *column_names,
    std::uint32_t column_count) {
    if (!context || (column_count != 0u && !column_names)) return false;

    std::string names = "N:";
    for (std::uint32_t column = 0u; column < column_count; ++column) {
        if (!column_names[column]) return false;
        if (column != 0u) names += "|";
        names += hex_encode(
            column_names[column], std::strlen(column_names[column]));
    }
    names += "\n";
    if (names.size() + k_query_result_header_max_bytes >
        k_query_result_wire_max_bytes) {
        return false;
    }
    context->column_count = column_count;
    context->names.swap(names);
    context->names_ready = true;
    return true;
}

bool prime_query_column_names(
    QueryWireContext *context,
    sqlite3_stmt *statement) {
    if (!context || !statement) return false;
    const int column_count_i = sqlite3_column_count(statement);
    if (column_count_i < 0) return false;
    const std::uint32_t column_count = static_cast<std::uint32_t>(column_count_i);
    std::vector<const char *> column_names;
    column_names.reserve(column_count);
    for (std::uint32_t column = 0u; column < column_count; ++column) {
        column_names.push_back(
            sqlite3_column_name(statement, static_cast<int>(column)));
    }
    return set_query_column_names(
        context,
        column_names.empty() ? nullptr : column_names.data(),
        column_count);
}

int append_query_row_wire(
    const char *const *column_names,
    const esdb_value *const *values,
    std::uint32_t column_count,
    void *user_data) {
    auto *context = static_cast<QueryWireContext *>(user_data);
    if (!context || !values || (column_count != 0u && !column_names)) return 1;
    try {
        if (!context->names_ready) {
            if (!set_query_column_names(context, column_names, column_count)) {
                context->failed = true;
                return 1;
            }
        } else if (context->column_count != column_count) {
            context->failed = true;
            return 1;
        }

        if (context->returned_rows >= context->max_rows) {
            context->truncated = true;
            return 1;
        }

        std::string row = "R:";
        for (std::uint32_t column = 0u; column < column_count; ++column) {
            std::string value;
            if (!value_wire(values[column], value) ||
                value.size() < 3u || value.compare(0u, 3u, "V1:") != 0) {
                context->failed = true;
                return 1;
            }
            if (column != 0u) row += "|";
            row += value.substr(3u);
        }
        row += "\n";

        if (context->names.size() + context->rows.size() + row.size() +
                k_query_result_header_max_bytes >
            k_query_result_wire_max_bytes) {
            context->truncated = true;
            return 1;
        }
        context->rows += row;
        ++context->returned_rows;
        return 0;
    } catch (...) {
        context->failed = true;
        return 1;
    }
}

struct ChangeWireContext {
    std::string body;
    bool failed = false;
};

struct RecordWireContext {
    std::string body;
    bool failed = false;
    std::uint32_t count = 0u;
};

int append_record_wire(
    const esdb_object_record *record,
    void *user_data) {
    auto *context = static_cast<RecordWireContext *>(user_data);
    if (!context || !record || !record->key || !record->value) return 1;
    try {
        std::string value;
        if (!value_wire(record->value, value) ||
            value.size() < 3u || value.compare(0u, 3u, "V1:") != 0) {
            context->failed = true;
            return 1;
        }
        context->body += u64_decimal(record->revision);
        context->body += ":";
        context->body += hex_encode(record->key, std::strlen(record->key));
        context->body += ":";
        context->body += value.substr(3u);
        context->body += "\n";
        ++context->count;
        return 0;
    } catch (...) {
        context->failed = true;
        return 1;
    }
}

int append_memory_record_wire(
    const esdb_store_record *record,
    void *user_data) {
    auto *context = static_cast<RecordWireContext *>(user_data);
    if (!context || !record || !record->key || !record->value) return 1;
    try {
        std::string value;
        if (!value_wire(record->value, value) ||
            value.size() < 3u || value.compare(0u, 3u, "V1:") != 0) {
            context->failed = true;
            return 1;
        }
        context->body += u64_decimal(record->revision);
        context->body += ":";
        context->body += hex_encode(record->key, std::strlen(record->key));
        context->body += ":";
        context->body += value.substr(3u);
        context->body += "\n";
        ++context->count;
        return 0;
    } catch (...) {
        context->failed = true;
        return 1;
    }
}

int append_change_wire(const esdb_object_change *change, void *user_data) {
    auto *context = static_cast<ChangeWireContext *>(user_data);
    if (!context || !change || !change->store_name || !change->key) return 1;
    try {
        const std::size_t store_size = std::strlen(change->store_name);
        const std::size_t key_size = std::strlen(change->key);
        context->body += u64_decimal(change->revision);
        context->body += ":";
        context->body += std::to_string(static_cast<unsigned>(change->operation));
        context->body += ":";
        context->body += std::to_string(static_cast<unsigned>(change->value_type));
        context->body += ":";
        context->body += hex_encode(change->store_name, store_size);
        context->body += ":";
        context->body += hex_encode(change->key, key_size);
        context->body += "\n";
        return 0;
    } catch (...) {
        context->failed = true;
        return 1;
    }
}

struct MemoryStoreGuard {
    esdb_store *store = nullptr;

    ~MemoryStoreGuard() noexcept {
        if (store) esdb_store_close(store);
    }

    MemoryStoreGuard() noexcept = default;
    MemoryStoreGuard(const MemoryStoreGuard &) = delete;
    MemoryStoreGuard &operator=(const MemoryStoreGuard &) = delete;
};

esdb_status open_memory_store_hex(
    const char *name_hex,
    MemoryStoreGuard &guard,
    std::string &decoded_name,
    esdb_error *error) {
    if (!name_hex || !decode_hex_cstring(name_hex, decoded_name, false)) {
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    return esdb_store_open(decoded_name.c_str(), &guard.store, error);
}

int append_memory_change_wire(
    const esdb_store_change *change,
    void *user_data) {
    auto *context = static_cast<ChangeWireContext *>(user_data);
    if (!context || !change) return 1;
    if (change->operation != ESDB_STORE_CHANGE_CLEAR && !change->key) return 1;
    try {
        context->body += u64_decimal(change->revision);
        context->body += ":";
        context->body += std::to_string(static_cast<unsigned>(change->operation));
        context->body += ":";
        context->body += std::to_string(static_cast<unsigned>(change->value_type));
        context->body += ":";
        if (change->key) {
            context->body += hex_encode(change->key, std::strlen(change->key));
        }
        context->body += "\n";
        return 0;
    } catch (...) {
        context->failed = true;
        return 1;
    }
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
    ESABI_SIGNATURE(stageHex, ESABI_SIG_STRING)
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
    ESABI_SIGNATURE(querySql, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(handleCount, ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(transactionBegin, ESABI_SIG_I32 ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(transactionCommit, ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(transactionRollback, ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(transactionActive, ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStoreEnsure, ESABI_SIG_I32 ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStorePutNumber, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32 ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStorePutText, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32 ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStoreGet, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStoreDelete, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStoreExists, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStoreCount, ESABI_SIG_I32 ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStoreScan, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStoreRevision, ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStoreChanges, ESABI_SIG_I32 ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(objectStorePrune, ESABI_SIG_I32 ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeDestroy, ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storePutNumber, ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32 ESABI_SIG_DOUBLE)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storePutText, ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32 ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storePatch, ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeGet, ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeDelete, ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeExists, ESABI_SIG_STRING ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeCount, ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeScan, ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeClear, ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeRevision, ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeRetainedFloor, ESABI_SIG_STRING)
    ESABI_SIGNATURE_SEPARATOR
    ESABI_SIGNATURE(storeChanges, ESABI_SIG_STRING ESABI_SIG_STRING ESABI_SIG_I32);

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
        std::lock_guard<esdb_detail::NoThrowMutex> table_lock(g_mutex);
        for (Slot &slot : g_slots) {
            std::lock_guard<esdb_detail::NoThrowMutex> operation_lock(
                slot.operation_mutex);
            if (slot.transaction) {
                esdb_transaction_destroy(slot.transaction);
                slot.transaction = nullptr;
            }
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

ESABI_DIRECT_FUNCTION(stageHex) {
    try {
        const char *encoded = argc == 1 ? esabi_arg_get_string(argv, argc, 0) : nullptr;
        std::string path;
        if (!encoded || !decode_hex_cstring(encoded, path, false)) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "stageHex expects non-empty ASCII hex encoding of a NUL-free UTF-8 path");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        g_staged_path = path;
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, 1);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to decode staged database path");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in stageHex");
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

        std::lock_guard<esdb_detail::NoThrowMutex> table_lock(g_mutex);
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
        std::unique_lock<esdb_detail::NoThrowMutex> table_lock(g_mutex);
        Slot *slot = lookup_slot_unlocked(handle);
        if (!slot) {
            set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        std::unique_lock<esdb_detail::NoThrowMutex> operation_lock(slot->operation_mutex);
        if (slot->transaction) {
            esdb_transaction_destroy(slot->transaction);
            slot->transaction = nullptr;
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
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
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
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
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


ESABI_DIRECT_FUNCTION(querySql) {
    try {
        std::int32_t handle = 0;
        std::int32_t max_rows_i32 = 0;
        const char *sql_hex = nullptr;
        const char *parameter_wire = nullptr;
        if (argc != 4 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !(sql_hex = get_string_arg(argv, argc, 1)) ||
            !(parameter_wire = get_string_arg(argv, argc, 2)) ||
            !get_i32_arg(argv, argc, 3, max_rows_i32) ||
            max_rows_i32 < 0 ||
            max_rows_i32 > static_cast<std::int32_t>(k_query_row_max)) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "querySql expects handle, SQL UTF-8 hex, P1 parameter packet, and maxRows 0..10000");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        const std::size_t sql_hex_size = std::strlen(sql_hex);
        if (sql_hex_size == 0u ||
            sql_hex_size > k_query_sql_max_bytes * 2u) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "querySql SQL exceeds the 1 MiB UTF-8 limit");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::string sql;
        if (!decode_hex_cstring(sql_hex, sql, false) ||
            sql.size() > k_query_sql_max_bytes) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "querySql SQL must be non-empty NUL-free UTF-8 bytes encoded as ASCII hex");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        QueryParameterGuard parameters;
        if (!parse_query_parameter_wire(parameter_wire, parameters)) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "querySql parameter packet is malformed or exceeds the P1 limits");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::vector<const esdb_value *> parameter_refs;
        parameter_refs.reserve(parameters.values.size());
        for (esdb_value *value : parameters.values) parameter_refs.push_back(value);

        QueryWireContext context;
        context.max_rows = static_cast<std::uint32_t>(max_rows_i32);
        std::uint64_t total_rows = 0u;
        std::uint64_t changes = 0u;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                set_last_error_unlocked(
                    ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }

            esdb_error error{};
            /*
             * esdb_query's row callback cannot supply result metadata when a
             * SELECT returns no rows. Prepare a metadata-only statement here
             * so the Q1 reply still has the deterministic column names/count.
             * Execution, binding, row values, and changes remain owned by
             * esdb_query below.
             */
            QueryMetadataStatementGuard metadata_statement;
            const char *metadata_tail = nullptr;
            sqlite3 *native_database = static_cast<sqlite3 *>(
                esdb_native_handle(slot->database));
            if (native_database && sqlite3_prepare_v3(
                    native_database,
                    sql.c_str(),
                    -1,
                    SQLITE_PREPARE_PERSISTENT,
                    &metadata_statement.statement,
                    &metadata_tail) == SQLITE_OK &&
                metadata_statement.statement) {
                if (!prime_query_column_names(&context, metadata_statement.statement)) {
                    context.failed = true;
                }
            }
            metadata_statement.finalize();

            const esdb_status status = esdb_query(
                slot->database,
                sql.c_str(),
                parameter_refs.empty() ? nullptr : parameter_refs.data(),
                static_cast<std::uint32_t>(parameter_refs.size()),
                append_query_row_wire,
                &context,
                &total_rows,
                &changes,
                &error);

            if (slot->transaction && !esdb_transaction_active(slot->transaction)) {
                esdb_transaction_destroy(slot->transaction);
                slot->transaction = nullptr;
            }

            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR, "ESDB typed SQL query failed", &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            if (context.failed) {
                set_last_error_unlocked(
                    ADAPTER_INTERNAL, "failed to encode typed SQL query result");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }

        if (!context.names_ready) {
            context.names = "N:\n";
            context.column_count = 0u;
        }
        const bool truncated =
            context.truncated ||
            total_rows > static_cast<std::uint64_t>(context.returned_rows);

        std::string wire =
            "Q1:" + std::to_string(context.column_count) +
            ":" + std::to_string(context.returned_rows) +
            ":" + u64_decimal(total_rows) +
            ":" + u64_decimal(changes) +
            ":" + (truncated ? "1" : "0") + "\n";
        wire += context.names;
        wire += context.rows;
        if (!set_return_string(retval, wire)) {
            esabi_value_set_undefined(retval);
        }
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "querySql allocation failed");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in querySql");
        esabi_value_set_undefined(retval);
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
            std::lock_guard<esdb_detail::NoThrowMutex> table_lock(g_mutex);
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

ESABI_DIRECT_FUNCTION(transactionBegin) {
    try {
        std::int32_t handle = 0;
        std::int32_t mode = 0;
        if (argc != 2 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !get_i32_arg(argv, argc, 1, mode) ||
            mode < static_cast<std::int32_t>(ESDB_TRANSACTION_DEFERRED) ||
            mode > static_cast<std::int32_t>(ESDB_TRANSACTION_EXCLUSIVE)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "transactionBegin expects handle and transaction mode");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
        Slot *slot = lock_slot(handle, operation_lock);
        if (!slot) {
            set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        if (slot->transaction) {
            set_last_error_unlocked(ADAPTER_DATABASE_ERROR, "a transaction is already active for this handle");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        esdb_error error{};
        esdb_transaction *transaction = nullptr;
        const esdb_status status = esdb_begin(
            slot->database,
            static_cast<esdb_transaction_mode>(mode),
            &transaction,
            &error);
        if (status != ESDB_OK) {
            set_last_error_unlocked(
                ADAPTER_DATABASE_ERROR, "ESDB failed to begin transaction", &error);
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        slot->transaction = transaction;
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, 1);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in transactionBegin");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(transactionCommit) {
    try {
        std::int32_t handle = 0;
        if (argc != 1 || !get_i32_arg(argv, argc, 0, handle) || handle <= 0) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "transactionCommit expects one positive integer handle");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
        Slot *slot = lock_slot(handle, operation_lock);
        if (!slot) {
            set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        if (!slot->transaction) {
            set_last_error_unlocked(ADAPTER_DATABASE_ERROR, "no transaction is active for this handle");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        esdb_error error{};
        const esdb_status status = esdb_commit(slot->transaction, &error);
        if (status != ESDB_OK) {
            set_last_error_unlocked(
                ADAPTER_DATABASE_ERROR, "ESDB failed to commit transaction", &error);
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        esdb_transaction_destroy(slot->transaction);
        slot->transaction = nullptr;
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, 1);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in transactionCommit");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(transactionRollback) {
    try {
        std::int32_t handle = 0;
        if (argc != 1 || !get_i32_arg(argv, argc, 0, handle) || handle <= 0) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "transactionRollback expects one positive integer handle");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
        Slot *slot = lock_slot(handle, operation_lock);
        if (!slot) {
            set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        if (!slot->transaction) {
            set_last_error_unlocked(ADAPTER_DATABASE_ERROR, "no transaction is active for this handle");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        esdb_error error{};
        const esdb_status status = esdb_rollback(slot->transaction, &error);
        if (status != ESDB_OK) {
            set_last_error_unlocked(
                ADAPTER_DATABASE_ERROR, "ESDB failed to roll back transaction", &error);
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        esdb_transaction_destroy(slot->transaction);
        slot->transaction = nullptr;
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, 1);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in transactionRollback");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(transactionActive) {
    try {
        std::int32_t handle = 0;
        if (argc != 1 || !get_i32_arg(argv, argc, 0, handle) || handle <= 0) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "transactionActive expects one positive integer handle");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
        Slot *slot = lock_slot(handle, operation_lock);
        if (!slot) {
            set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        const bool active =
            slot->transaction && esdb_transaction_active(slot->transaction) != 0;
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, active ? 1 : 0);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in transactionActive");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStoreEnsure) {
    try {
        std::int32_t handle = 0;
        const char *store_hex = argc == 2 ? get_string_arg(argv, argc, 1) : nullptr;
        std::string store_name;
        if (argc != 2 || !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !store_hex || !decode_hex_cstring(store_hex, store_name, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "objectStoreEnsure expects handle and UTF-8 store name encoded as ASCII hex");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
        Slot *slot = lock_slot(handle, operation_lock);
        if (!slot) {
            set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        esdb_error error{};
        const esdb_status status =
            esdb_object_store_ensure(slot->database, store_name.c_str(), &error);
        if (status != ESDB_OK) {
            set_last_error_unlocked(
                ADAPTER_DATABASE_ERROR, "ESDB failed to ensure Store", &error);
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, 1);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStoreEnsure");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStorePutNumber) {
    try {
        std::int32_t handle = 0;
        std::int32_t type = -1;
        double number = 0.0;
        std::string store_name;
        std::string key;
        if (argc != 5 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !decode_store_key_args(argv, argc, 1, 2, store_name, key) ||
            !get_i32_arg(argv, argc, 3, type) ||
            !get_double_arg(argv, argc, 4, number)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "objectStorePutNumber expects handle, hex-encoded UTF-8 store/key, type, and number");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        esdb_value *value = nullptr;
        esdb_error value_error{};
        const esdb_status value_status =
            make_number_value(type, number, &value, &value_error);
        if (value_status != ESDB_OK || !value) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "numeric value does not match the requested ESDB value type",
                value_error.status == ESDB_OK ? nullptr : &value_error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::uint64_t revision = 0u;
        esdb_error error{};
        esdb_status status = ESDB_ERR_INTERNAL;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                esdb_value_destroy(value);
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            status = esdb_object_store_put(
                slot->database, store_name.c_str(), key.c_str(), value, &revision, &error);
            esdb_value_destroy(value);
            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR, "ESDB Store put failed", &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }
        set_return_string(retval, u64_decimal(revision));
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to allocate Store numeric value");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStorePutNumber");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStorePutText) {
    try {
        std::int32_t handle = 0;
        std::int32_t type = -1;
        std::string store_name;
        std::string key;
        const char *payload = argc == 5 ? get_string_arg(argv, argc, 4) : nullptr;
        if (argc != 5 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !decode_store_key_args(argv, argc, 1, 2, store_name, key) ||
            !get_i32_arg(argv, argc, 3, type) || !payload) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "objectStorePutText expects handle, hex-encoded UTF-8 store/key, type, and payload");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        esdb_value *value = nullptr;
        esdb_error value_error{};
        const esdb_status value_status =
            make_text_value(type, payload, &value, &value_error);
        if (value_status != ESDB_OK || !value) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "text payload does not match the requested ESDB value type",
                value_error.status == ESDB_OK ? nullptr : &value_error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::uint64_t revision = 0u;
        esdb_error error{};
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                esdb_value_destroy(value);
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            const esdb_status status = esdb_object_store_put(
                slot->database, store_name.c_str(), key.c_str(), value, &revision, &error);
            esdb_value_destroy(value);
            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR, "ESDB Store put failed", &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }
        set_return_string(retval, u64_decimal(revision));
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to allocate Store text value");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStorePutText");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStoreGet) {
    try {
        std::int32_t handle = 0;
        std::string store_name;
        std::string key;
        if (argc != 3 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !decode_store_key_args(argv, argc, 1, 2, store_name, key)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "objectStoreGet expects handle and hex-encoded UTF-8 store/key");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        esdb_value *value = nullptr;
        std::string wire;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }

            esdb_error error{};
            const esdb_status status =
                esdb_object_store_get(slot->database, store_name.c_str(), key.c_str(), &value, &error);
            if (status == ESDB_ERR_NOT_FOUND) {
                clear_last_error_unlocked();
                wire = "V1:-1:";
            } else if (status != ESDB_OK || !value) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR, "ESDB Store get failed", &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            } else {
                const bool encoded = value_wire(value, wire);
                esdb_value_destroy(value);
                value = nullptr;
                if (!encoded) {
                    set_last_error_unlocked(
                        ADAPTER_INTERNAL, "failed to encode ESDB Store value");
                    esabi_value_set_undefined(retval);
                    return ESABI_OK;
                }
                clear_last_error_unlocked();
            }
        }
        set_return_string(retval, wire);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to encode Store get result");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStoreGet");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStoreDelete) {
    try {
        std::int32_t handle = 0;
        std::string store_name;
        std::string key;
        if (argc != 3 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !decode_store_key_args(argv, argc, 1, 2, store_name, key)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "objectStoreDelete expects handle and hex-encoded UTF-8 store/key");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        int deleted = 0;
        std::uint64_t revision = 0u;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            esdb_error error{};
            const esdb_status status = esdb_object_store_delete(
                slot->database, store_name.c_str(), key.c_str(), &deleted, &revision, &error);
            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR, "ESDB Store delete failed", &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }
        set_return_string(
            retval,
            std::string("V1:") + (deleted ? "1:" : "0:") + u64_decimal(revision));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStoreDelete");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStoreExists) {
    try {
        std::int32_t handle = 0;
        std::string store_name;
        std::string key;
        if (argc != 3 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !decode_store_key_args(argv, argc, 1, 2, store_name, key)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "objectStoreExists expects handle and hex-encoded UTF-8 store/key");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        int exists = 0;
        std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
        Slot *slot = lock_slot(handle, operation_lock);
        if (!slot) {
            set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        esdb_error error{};
        const esdb_status status =
            esdb_object_store_exists(slot->database, store_name.c_str(), key.c_str(), &exists, &error);
        if (status != ESDB_OK) {
            set_last_error_unlocked(
                ADAPTER_DATABASE_ERROR, "ESDB Store exists failed", &error);
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        clear_last_error_unlocked();
        esabi_value_set_bool(retval, exists ? 1 : 0);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStoreExists");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStoreCount) {
    try {
        std::int32_t handle = 0;
        const char *store_hex = argc == 2 ? get_string_arg(argv, argc, 1) : nullptr;
        std::string store_name;
        if (argc != 2 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !store_hex || !decode_hex_cstring(store_hex, store_name, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "objectStoreCount expects handle and UTF-8 store name encoded as ASCII hex");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::uint64_t count = 0u;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            esdb_error error{};
            const esdb_status status =
                esdb_object_store_count(slot->database, store_name.c_str(), &count, &error);
            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR, "ESDB Store count failed", &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }
        set_return_string(retval, u64_decimal(count));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStoreCount");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStoreScan) {
    try {
        std::int32_t handle = 0;
        std::int32_t limit = 0;
        const char *store_hex = argc == 4 ? get_string_arg(argv, argc, 1) : nullptr;
        const char *after_hex = argc == 4 ? get_string_arg(argv, argc, 2) : nullptr;
        std::string store_name;
        std::string after_key;
        bool no_after = false;

        if (after_hex) {
            no_after = after_hex[0] == '\0';
            if (!no_after && !decode_hex_cstring(after_hex, after_key, false)) {
                after_hex = nullptr;
            }
        }

        if (argc != 4 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !store_hex || !decode_hex_cstring(store_hex, store_name, false) ||
            !after_hex ||
            !get_i32_arg(argv, argc, 3, limit) ||
            limit < 0 || static_cast<std::uint32_t>(limit) > ESDB_OBJECT_SCAN_LIMIT_MAX) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "objectStoreScan expects handle, Store hex, optional after-key hex, and bounded limit");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        RecordWireContext context;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                set_last_error_unlocked(
                    ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }

            std::uint32_t count = 0u;
            esdb_error error{};
            const esdb_status status = esdb_object_store_scan(
                slot->database,
                store_name.c_str(),
                no_after ? nullptr : after_key.c_str(),
                static_cast<std::uint32_t>(limit),
                append_record_wire,
                &context,
                &count,
                &error);
            if (status != ESDB_OK || context.failed || count != context.count) {
                set_last_error_unlocked(
                    context.failed ? ADAPTER_OUT_OF_MEMORY : ADAPTER_DATABASE_ERROR,
                    context.failed
                        ? "failed to serialize ObjectStore scan window"
                        : "ESDB ObjectStore scan failed",
                    status == ESDB_OK ? nullptr : &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }

        set_return_string(
            retval,
            "V1:" + std::to_string(context.count) + "\n" + context.body);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(
            ADAPTER_OUT_OF_MEMORY, "failed to serialize ObjectStore scan window");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStoreScan");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStoreRevision) {
    try {
        std::int32_t handle = 0;
        if (argc != 1 || !get_i32_arg(argv, argc, 0, handle) || handle <= 0) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "objectStoreRevision expects one positive integer handle");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::uint64_t revision = 0u;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            esdb_error error{};
            const esdb_status status =
                esdb_object_store_revision(slot->database, &revision, &error);
            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR, "ESDB Store revision failed", &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }
        set_return_string(retval, u64_decimal(revision));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStoreRevision");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStoreChanges) {
    try {
        std::int32_t handle = 0;
        std::int32_t limit = 0;
        const char *store_hex = argc == 4 ? get_string_arg(argv, argc, 1) : nullptr;
        const char *after_text = argc == 4 ? get_string_arg(argv, argc, 2) : nullptr;
        std::string store_name;
        bool all_stores = false;
        if (store_hex) {
            all_stores = store_hex[0] == '\0';
            if (!all_stores && !decode_hex_cstring(store_hex, store_name, false)) {
                store_hex = nullptr;
            }
        }
        std::uint64_t after_revision = 0u;
        if (argc != 4 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !store_hex ||
            !after_text || !parse_revision_decimal(after_text, after_revision) ||
            !get_i32_arg(argv, argc, 3, limit) ||
            limit < 0 || static_cast<std::uint32_t>(limit) > ESDB_OBJECT_CHANGE_LIMIT_MAX) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "objectStoreChanges expects handle, optional hex-encoded UTF-8 store name, exact revision string, and bounded limit");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        ChangeWireContext context;
        std::uint64_t last_revision = after_revision;
        std::uint32_t count = 0u;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            esdb_error error{};
            const esdb_status status = esdb_object_store_changes_since(
                slot->database,
                all_stores ? nullptr : store_name.c_str(),
                after_revision,
                static_cast<std::uint32_t>(limit),
                append_change_wire,
                &context,
                &last_revision,
                &count,
                &error);
            if (status != ESDB_OK || context.failed) {
                set_last_error_unlocked(
                    context.failed ? ADAPTER_OUT_OF_MEMORY : ADAPTER_DATABASE_ERROR,
                    context.failed
                        ? "failed to serialize Store change window"
                        : "ESDB Store changes query failed",
                    status == ESDB_OK ? nullptr : &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }

        std::string wire =
            "V1:" + u64_decimal(last_revision) + ":" +
            std::to_string(count) + "\n" + context.body;
        set_return_string(retval, wire);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to serialize Store change window");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStoreChanges");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(objectStorePrune) {
    try {
        std::int32_t handle = 0;
        const char *through_text = argc == 2 ? get_string_arg(argv, argc, 1) : nullptr;
        std::uint64_t through_revision = 0u;
        if (argc != 2 ||
            !get_i32_arg(argv, argc, 0, handle) || handle <= 0 ||
            !through_text || !parse_revision_decimal(through_text, through_revision)) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "objectStorePrune expects handle and exact revision string");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::uint64_t deleted = 0u;
        {
            std::unique_lock<esdb_detail::NoThrowMutex> operation_lock;
            Slot *slot = lock_slot(handle, operation_lock);
            if (!slot) {
                set_last_error_unlocked(ADAPTER_INVALID_HANDLE, "database handle is stale or invalid");
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            esdb_error error{};
            const esdb_status status = esdb_object_store_prune_changes(
                slot->database, through_revision, &deleted, &error);
            if (status != ESDB_OK) {
                set_last_error_unlocked(
                    ADAPTER_DATABASE_ERROR, "ESDB Store prune failed", &error);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            clear_last_error_unlocked();
        }
        set_return_string(retval, u64_decimal(deleted));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in objectStorePrune");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}


ESABI_DIRECT_FUNCTION(storeDestroy) {
    try {
        const char *name_hex = argc == 1 ? get_string_arg(argv, argc, 0) : nullptr;
        std::string name;
        if (!name_hex || !decode_hex_cstring(name_hex, name, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeDestroy expects a hex-encoded UTF-8 Store name");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        esdb_error error{};
        const esdb_status status = esdb_store_destroy(name.c_str(), &error);
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "ESDB memory Store destroy failed", &error);
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        clear_last_error();
        esabi_value_set_bool(retval, 1);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeDestroy");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storePutNumber) {
    esdb_value *value = nullptr;
    try {
        const char *name_hex = argc == 4 ? get_string_arg(argv, argc, 0) : nullptr;
        const char *key_hex = argc == 4 ? get_string_arg(argv, argc, 1) : nullptr;
        std::int32_t type = -1;
        double number = 0.0;
        std::string name;
        std::string key;
        if (argc != 4 || !name_hex || !key_hex ||
            !decode_hex_cstring(name_hex, name, false) ||
            !decode_hex_cstring(key_hex, key, false) ||
            !get_i32_arg(argv, argc, 2, type) ||
            !get_double_arg(argv, argc, 3, number)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storePutNumber expects Store/key hex, type, and number");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        esdb_error error{};
        const esdb_status value_status =
            make_number_value(type, number, &value, &error);
        if (value_status != ESDB_OK || !value) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "invalid numeric Store value",
                           error.status == ESDB_OK ? nullptr : &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        MemoryStoreGuard guard;
        esdb_error store_error{};
        const esdb_status open_status =
            esdb_store_open(name.c_str(), &guard.store, &store_error);
        if (open_status != ESDB_OK) {
            esdb_value_destroy(value);
            value = nullptr;
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &store_error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::uint64_t revision = 0u;
        const esdb_status status =
            esdb_store_put(guard.store, key.c_str(), value, &revision, &store_error);
        esdb_value_destroy(value);
        value = nullptr;
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store put failed", &store_error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(retval, u64_decimal(revision));
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        if (value) esdb_value_destroy(value);
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to allocate memory Store value");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        if (value) esdb_value_destroy(value);
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storePutNumber");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storePutText) {
    esdb_value *value = nullptr;
    try {
        const char *name_hex = argc == 4 ? get_string_arg(argv, argc, 0) : nullptr;
        const char *key_hex = argc == 4 ? get_string_arg(argv, argc, 1) : nullptr;
        const char *payload = argc == 4 ? get_string_arg(argv, argc, 3) : nullptr;
        std::int32_t type = -1;
        std::string name;
        std::string key;
        if (argc != 4 || !name_hex || !key_hex || !payload ||
            !decode_hex_cstring(name_hex, name, false) ||
            !decode_hex_cstring(key_hex, key, false) ||
            !get_i32_arg(argv, argc, 2, type)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storePutText expects Store/key hex, type, and payload");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        esdb_error error{};
        const esdb_status value_status =
            make_text_value(type, payload, &value, &error);
        if (value_status != ESDB_OK || !value) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "invalid text Store value",
                           error.status == ESDB_OK ? nullptr : &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        MemoryStoreGuard guard;
        esdb_error store_error{};
        const esdb_status open_status =
            esdb_store_open(name.c_str(), &guard.store, &store_error);
        if (open_status != ESDB_OK) {
            esdb_value_destroy(value);
            value = nullptr;
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &store_error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::uint64_t revision = 0u;
        const esdb_status status =
            esdb_store_put(guard.store, key.c_str(), value, &revision, &store_error);
        esdb_value_destroy(value);
        value = nullptr;
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store put failed", &store_error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(retval, u64_decimal(revision));
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        if (value) esdb_value_destroy(value);
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to allocate memory Store value");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        if (value) esdb_value_destroy(value);
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storePutText");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storePatch) {
    std::vector<esdb_value *> values;
    try {
        const char *name_hex = argc == 2 ? get_string_arg(argv, argc, 0) : nullptr;
        const char *wire_text = argc == 2 ? get_string_arg(argv, argc, 1) : nullptr;
        std::string name;
        if (argc != 2 || !name_hex || !wire_text ||
            !decode_hex_cstring(name_hex, name, false)) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "storePatch expects Store name hex and patch wire");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        const std::string wire(wire_text);
        if (wire.size() > 16u * 1024u * 1024u ||
            wire.compare(0u, 3u, "P1:") != 0) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "storePatch wire is invalid or exceeds 16 MiB");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        const std::size_t header_end = wire.find('\n');
        const std::string count_text = wire.substr(
            3u,
            header_end == std::string::npos
                ? std::string::npos
                : header_end - 3u);
        errno = 0;
        char *count_end = nullptr;
        const unsigned long parsed_count =
            std::strtoul(count_text.c_str(), &count_end, 10);
        if (errno == ERANGE || !count_end || *count_end != '\0' ||
            parsed_count > ESDB_STORE_PATCH_MAX_ENTRIES) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT, "storePatch count is invalid");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        const std::uint32_t count =
            static_cast<std::uint32_t>(parsed_count);

        std::vector<std::string> keys;
        std::vector<esdb_store_patch_entry> entries;
        keys.reserve(count);
        values.reserve(count);
        entries.reserve(count);

        std::size_t cursor =
            header_end == std::string::npos ? wire.size() : header_end + 1u;
        for (std::uint32_t index = 0u; index < count; ++index) {
            const std::size_t line_end = wire.find('\n', cursor);
            const std::size_t end =
                line_end == std::string::npos ? wire.size() : line_end;
            const std::string line = wire.substr(cursor, end - cursor);
            cursor = line_end == std::string::npos ? wire.size() : line_end + 1u;

            const std::size_t first = line.find(':');
            const std::size_t second =
                first == std::string::npos
                    ? std::string::npos
                    : line.find(':', first + 1u);
            const std::size_t third =
                second == std::string::npos
                    ? std::string::npos
                    : line.find(':', second + 1u);
            if (first == std::string::npos ||
                second == std::string::npos ||
                third == std::string::npos ||
                second == first + 1u ||
                third != second + 2u) {
                set_last_error(
                    ADAPTER_BAD_ARGUMENT,
                    "storePatch contains a malformed entry");
                for (esdb_value *value : values) esdb_value_destroy(value);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }

            const std::string key_hex = line.substr(0u, first);
            std::string key;
            if (!decode_hex_cstring(key_hex.c_str(), key, false)) {
                set_last_error(
                    ADAPTER_BAD_ARGUMENT,
                    "storePatch contains an invalid key");
                for (esdb_value *value : values) esdb_value_destroy(value);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }

            const std::string type_text =
                line.substr(first + 1u, second - first - 1u);
            errno = 0;
            char *type_end = nullptr;
            const long type_long =
                std::strtol(type_text.c_str(), &type_end, 10);
            if (errno == ERANGE || !type_end || *type_end != '\0' ||
                type_long < ESDB_VALUE_NULL ||
                type_long > ESDB_VALUE_OBJECT) {
                set_last_error(
                    ADAPTER_BAD_ARGUMENT,
                    "storePatch contains an invalid value type");
                for (esdb_value *value : values) esdb_value_destroy(value);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }

            const char lane = line[second + 1u];
            const std::string payload = line.substr(third + 1u);
            esdb_value *value = nullptr;
            esdb_error value_error{};
            esdb_status value_status = ESDB_ERR_INVALID_ARGUMENT;
            if (lane == 'N') {
                double number = 0.0;
                if (parse_double_decimal(payload.c_str(), number)) {
                    value_status = make_number_value(
                        static_cast<std::int32_t>(type_long),
                        number,
                        &value,
                        &value_error);
                }
            } else if (lane == 'T') {
                value_status = make_text_value(
                    static_cast<std::int32_t>(type_long),
                    payload.c_str(),
                    &value,
                    &value_error);
            }
            if (value_status != ESDB_OK || !value) {
                set_last_error(
                    ADAPTER_BAD_ARGUMENT,
                    "storePatch contains an invalid value payload",
                    value_error.status == ESDB_OK ? nullptr : &value_error);
                for (esdb_value *owned : values) esdb_value_destroy(owned);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }

            keys.emplace_back(std::move(key));
            values.emplace_back(value);
        }

        while (cursor < wire.size()) {
            if (wire[cursor] != '\n' && wire[cursor] != '\r') {
                set_last_error(
                    ADAPTER_BAD_ARGUMENT,
                    "storePatch has trailing data after declared entries");
                for (esdb_value *value : values) esdb_value_destroy(value);
                esabi_value_set_undefined(retval);
                return ESABI_OK;
            }
            ++cursor;
        }

        for (std::uint32_t index = 0u; index < count; ++index) {
            esdb_store_patch_entry entry{};
            entry.key = keys[index].c_str();
            entry.value = values[index];
            entries.emplace_back(entry);
        }

        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(
                ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            for (esdb_value *value : values) esdb_value_destroy(value);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::uint64_t revision = 0u;
        const esdb_status status = esdb_store_patch(
            guard.store,
            entries.empty() ? nullptr : entries.data(),
            count,
            &revision,
            &error);
        for (esdb_value *value : values) esdb_value_destroy(value);
        values.clear();

        if (status != ESDB_OK) {
            set_last_error(
                ADAPTER_DATABASE_ERROR, "memory Store patch failed", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        clear_last_error();
        set_return_string(retval, u64_decimal(revision));
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        for (esdb_value *value : values) esdb_value_destroy(value);
        set_last_error(
            ADAPTER_OUT_OF_MEMORY, "failed to allocate memory Store patch");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        for (esdb_value *value : values) esdb_value_destroy(value);
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storePatch");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeGet) {
    try {
        const char *name_hex = argc == 2 ? get_string_arg(argv, argc, 0) : nullptr;
        const char *key_hex = argc == 2 ? get_string_arg(argv, argc, 1) : nullptr;
        std::string name;
        std::string key;
        if (argc != 2 || !name_hex || !key_hex ||
            !decode_hex_cstring(name_hex, name, false) ||
            !decode_hex_cstring(key_hex, key, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeGet expects Store/key hex");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        esdb_value *value = nullptr;
        const esdb_status status =
            esdb_store_get(guard.store, key.c_str(), &value, &error);
        if (status == ESDB_ERR_NOT_FOUND) {
            clear_last_error();
            set_return_string(retval, "V1:-1:");
            return ESABI_OK;
        }
        if (status != ESDB_OK || !value) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store get failed", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        std::string wire;
        const bool encoded = value_wire(value, wire);
        esdb_value_destroy(value);
        if (!encoded) {
            set_last_error(ADAPTER_INTERNAL, "failed to encode memory Store value");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(retval, wire);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to encode memory Store get result");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeGet");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeDelete) {
    try {
        const char *name_hex = argc == 2 ? get_string_arg(argv, argc, 0) : nullptr;
        const char *key_hex = argc == 2 ? get_string_arg(argv, argc, 1) : nullptr;
        std::string name;
        std::string key;
        if (argc != 2 || !name_hex || !key_hex ||
            !decode_hex_cstring(name_hex, name, false) ||
            !decode_hex_cstring(key_hex, key, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeDelete expects Store/key hex");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        int deleted = 0;
        std::uint64_t revision = 0u;
        const esdb_status status =
            esdb_store_delete(guard.store, key.c_str(), &deleted, &revision, &error);
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store delete failed", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(
            retval,
            std::string("V1:") + (deleted ? "1:" : "0:") + u64_decimal(revision));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeDelete");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeExists) {
    try {
        const char *name_hex = argc == 2 ? get_string_arg(argv, argc, 0) : nullptr;
        const char *key_hex = argc == 2 ? get_string_arg(argv, argc, 1) : nullptr;
        std::string name;
        std::string key;
        if (argc != 2 || !name_hex || !key_hex ||
            !decode_hex_cstring(name_hex, name, false) ||
            !decode_hex_cstring(key_hex, key, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeExists expects Store/key hex");
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }

        int exists = 0;
        const esdb_status status =
            esdb_store_exists(guard.store, key.c_str(), &exists, &error);
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store exists failed", &error);
            esabi_value_set_bool(retval, 0);
            return ESABI_OK;
        }
        clear_last_error();
        esabi_value_set_bool(retval, exists ? 1 : 0);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeExists");
        esabi_value_set_bool(retval, 0);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeCount) {
    try {
        const char *name_hex = argc == 1 ? get_string_arg(argv, argc, 0) : nullptr;
        std::string name;
        if (argc != 1 || !name_hex || !decode_hex_cstring(name_hex, name, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeCount expects Store name hex");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        std::uint64_t count = 0u;
        const esdb_status status = esdb_store_count(guard.store, &count, &error);
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store count failed", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(retval, u64_decimal(count));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeCount");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeScan) {
    try {
        const char *name_hex = argc == 3 ? get_string_arg(argv, argc, 0) : nullptr;
        const char *after_hex = argc == 3 ? get_string_arg(argv, argc, 1) : nullptr;
        std::int32_t limit = 0;
        std::string name;
        std::string after_key;
        bool no_after = false;

        if (after_hex) {
            no_after = after_hex[0] == '\0';
            if (!no_after && !decode_hex_cstring(after_hex, after_key, false)) {
                after_hex = nullptr;
            }
        }

        if (argc != 3 ||
            !name_hex || !decode_hex_cstring(name_hex, name, false) ||
            !after_hex ||
            !get_i32_arg(argv, argc, 2, limit) ||
            limit < 0 || static_cast<std::uint32_t>(limit) > ESDB_STORE_SCAN_LIMIT_MAX) {
            set_last_error(
                ADAPTER_BAD_ARGUMENT,
                "storeScan expects Store hex, optional after-key hex, and bounded limit");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(
                ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        RecordWireContext context;
        std::uint32_t count = 0u;
        const esdb_status status = esdb_store_scan(
            guard.store,
            no_after ? nullptr : after_key.c_str(),
            static_cast<std::uint32_t>(limit),
            append_memory_record_wire,
            &context,
            &count,
            &error);
        if (status != ESDB_OK || context.failed || count != context.count) {
            set_last_error(
                context.failed ? ADAPTER_OUT_OF_MEMORY : ADAPTER_DATABASE_ERROR,
                context.failed
                    ? "failed to serialize memory Store scan window"
                    : "memory Store scan failed",
                status == ESDB_OK ? nullptr : &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        clear_last_error();
        set_return_string(
            retval,
            "V1:" + std::to_string(context.count) + "\n" + context.body);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(
            ADAPTER_OUT_OF_MEMORY, "failed to serialize memory Store scan window");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeScan");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeClear) {
    try {
        const char *name_hex = argc == 1 ? get_string_arg(argv, argc, 0) : nullptr;
        std::string name;
        if (argc != 1 || !name_hex || !decode_hex_cstring(name_hex, name, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeClear expects Store name hex");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        int cleared = 0;
        std::uint64_t revision = 0u;
        const esdb_status status =
            esdb_store_clear(guard.store, &cleared, &revision, &error);
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store clear failed", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(
            retval,
            std::string("V1:") + (cleared ? "1:" : "0:") + u64_decimal(revision));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeClear");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeRevision) {
    try {
        const char *name_hex = argc == 1 ? get_string_arg(argv, argc, 0) : nullptr;
        std::string name;
        if (argc != 1 || !name_hex || !decode_hex_cstring(name_hex, name, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeRevision expects Store name hex");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        std::uint64_t revision = 0u;
        const esdb_status status =
            esdb_store_revision(guard.store, &revision, &error);
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store revision failed", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(retval, u64_decimal(revision));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeRevision");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeRetainedFloor) {
    try {
        const char *name_hex = argc == 1 ? get_string_arg(argv, argc, 0) : nullptr;
        std::string name;
        if (argc != 1 || !name_hex || !decode_hex_cstring(name_hex, name, false)) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeRetainedFloor expects Store name hex");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        std::uint64_t revision = 0u;
        const esdb_status status =
            esdb_store_retained_floor(guard.store, &revision, &error);
        if (status != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "memory Store retained-floor query failed", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }
        clear_last_error();
        set_return_string(retval, u64_decimal(revision));
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeRetainedFloor");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}

ESABI_DIRECT_FUNCTION(storeChanges) {
    try {
        const char *name_hex = argc == 3 ? get_string_arg(argv, argc, 0) : nullptr;
        const char *after_text = argc == 3 ? get_string_arg(argv, argc, 1) : nullptr;
        std::int32_t limit = 0;
        std::uint64_t after_revision = 0u;
        std::string name;
        if (argc != 3 || !name_hex ||
            !decode_hex_cstring(name_hex, name, false) ||
            !after_text || !parse_revision_decimal(after_text, after_revision) ||
            !get_i32_arg(argv, argc, 2, limit) ||
            limit < 0 || static_cast<std::uint32_t>(limit) > ESDB_STORE_CHANGE_LIMIT_MAX) {
            set_last_error(ADAPTER_BAD_ARGUMENT, "storeChanges expects Store hex, exact revision, and bounded limit");
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        MemoryStoreGuard guard;
        esdb_error error{};
        if (esdb_store_open(name.c_str(), &guard.store, &error) != ESDB_OK) {
            set_last_error(ADAPTER_DATABASE_ERROR, "failed to open memory Store", &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        ChangeWireContext context;
        std::uint64_t last_revision = after_revision;
        std::uint32_t count = 0u;
        const esdb_status status = esdb_store_changes_since(
            guard.store,
            after_revision,
            static_cast<std::uint32_t>(limit),
            append_memory_change_wire,
            &context,
            &last_revision,
            &count,
            &error);
        if (status != ESDB_OK || context.failed) {
            set_last_error(
                context.failed ? ADAPTER_OUT_OF_MEMORY : ADAPTER_DATABASE_ERROR,
                context.failed
                    ? "failed to serialize memory Store change window"
                    : "memory Store changes query failed",
                status == ESDB_OK ? nullptr : &error);
            esabi_value_set_undefined(retval);
            return ESABI_OK;
        }

        clear_last_error();
        std::string wire =
            "V1:" + u64_decimal(last_revision) + ":" +
            std::to_string(count) + "\n" + context.body;
        set_return_string(retval, wire);
        return ESABI_OK;
    } catch (const std::bad_alloc &) {
        set_last_error(ADAPTER_OUT_OF_MEMORY, "failed to serialize memory Store changes");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    } catch (...) {
        set_last_error(ADAPTER_INTERNAL, "unexpected exception in storeChanges");
        esabi_value_set_undefined(retval);
        return ESABI_OK;
    }
}
