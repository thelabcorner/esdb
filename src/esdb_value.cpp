#include "esdb_internal.hpp"

#include <cmath>
#include <cstring>
#include <new>

static void append_le32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.resize(4);
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xffu);
}

static void append_le64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    out.resize(8);
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xffu);
}

static std::uint32_t read_le32(const std::vector<std::uint8_t> &in) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(in[i]) << (i * 8u);
    return value;
}

static std::uint64_t read_le64(const std::vector<std::uint8_t> &in) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(in[i]) << (i * 8u);
    return value;
}

static esdb_status allocate_value(esdb_value_type type, esdb_value **out_value, esdb_error *error) {
    esdb_detail::clear_error(error);
    if (!out_value) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_VALUE, nullptr, SQLITE_MISUSE, "out_value is required");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    esdb_value *value = new (std::nothrow) esdb_value();
    if (!value) {
        esdb_detail::set_error(error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_VALUE, nullptr, SQLITE_NOMEM, "value allocation failed");
        return ESDB_ERR_OUT_OF_MEMORY;
    }
    value->type = type;
    *out_value = value;
    return ESDB_OK;
}

esdb_status esdb_value_create_null(esdb_value **out_value, esdb_error *error) {
    return allocate_value(ESDB_VALUE_NULL, out_value, error);
}

esdb_status esdb_value_create_bool(int input, esdb_value **out_value, esdb_error *error) {
    esdb_status status = allocate_value(ESDB_VALUE_BOOL, out_value, error);
    if (status != ESDB_OK) return status;
    try {
        (*out_value)->payload.push_back(input ? 1u : 0u);
    } catch (...) {
        esdb_value_destroy(*out_value);
        *out_value = nullptr;
        esdb_detail::set_error(error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_VALUE, nullptr, SQLITE_NOMEM,
                               "boolean payload allocation failed");
        return ESDB_ERR_OUT_OF_MEMORY;
    }
    return ESDB_OK;
}

esdb_status esdb_value_create_int32(int32_t input, esdb_value **out_value, esdb_error *error) {
    esdb_status status = allocate_value(ESDB_VALUE_INT32, out_value, error);
    if (status != ESDB_OK) return status;
    try {
        append_le32((*out_value)->payload, static_cast<std::uint32_t>(input));
    } catch (...) {
        esdb_value_destroy(*out_value);
        *out_value = nullptr;
        esdb_detail::set_error(error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_VALUE, nullptr, SQLITE_NOMEM,
                               "int32 payload allocation failed");
        return ESDB_ERR_OUT_OF_MEMORY;
    }
    return ESDB_OK;
}

esdb_status esdb_value_create_int64(int64_t input, esdb_value **out_value, esdb_error *error) {
    esdb_status status = allocate_value(ESDB_VALUE_INT64, out_value, error);
    if (status != ESDB_OK) return status;
    try {
        append_le64((*out_value)->payload, static_cast<std::uint64_t>(input));
    } catch (...) {
        esdb_value_destroy(*out_value);
        *out_value = nullptr;
        esdb_detail::set_error(error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_VALUE, nullptr, SQLITE_NOMEM,
                               "int64 payload allocation failed");
        return ESDB_ERR_OUT_OF_MEMORY;
    }
    return ESDB_OK;
}

esdb_status esdb_value_create_double(double input, esdb_value **out_value, esdb_error *error) {
    static_assert(sizeof(double) == sizeof(std::uint64_t), "ESDB requires IEEE-754 binary64-sized double");
    esdb_status status = allocate_value(ESDB_VALUE_DOUBLE, out_value, error);
    if (status != ESDB_OK) return status;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &input, sizeof(bits));
    try {
        append_le64((*out_value)->payload, bits);
    } catch (...) {
        esdb_value_destroy(*out_value);
        *out_value = nullptr;
        esdb_detail::set_error(error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_VALUE, nullptr, SQLITE_NOMEM,
                               "double payload allocation failed");
        return ESDB_ERR_OUT_OF_MEMORY;
    }
    return ESDB_OK;
}

esdb_status esdb_value_create_text(esdb_value_type type, const char *utf8, uint64_t size, esdb_value **out_value, esdb_error *error) {
    esdb_detail::clear_error(error);
    if (type != ESDB_VALUE_UTF8 && type != ESDB_VALUE_ARRAY && type != ESDB_VALUE_OBJECT) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_VALUE, nullptr, SQLITE_MISUSE, "text constructor requires UTF8, ARRAY, or OBJECT type");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    if (size > static_cast<uint64_t>(SIZE_MAX) || (size > 0 && !utf8) || !esdb_detail::valid_utf8(utf8, size)) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_VALUE, nullptr, SQLITE_MISMATCH, "invalid UTF-8 payload");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    esdb_status status = allocate_value(type, out_value, error);
    if (status != ESDB_OK) return status;
    try {
        if (size > 0) {
            const auto *begin = reinterpret_cast<const std::uint8_t *>(utf8);
            (*out_value)->payload.assign(begin, begin + static_cast<std::size_t>(size));
        }
    } catch (...) {
        esdb_value_destroy(*out_value);
        *out_value = nullptr;
        esdb_detail::set_error(error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_VALUE, nullptr, SQLITE_NOMEM, "text payload allocation failed");
        return ESDB_ERR_OUT_OF_MEMORY;
    }
    return ESDB_OK;
}

esdb_status esdb_value_create_bytes(const void *bytes, uint64_t size, esdb_value **out_value, esdb_error *error) {
    esdb_detail::clear_error(error);
    if (size > static_cast<uint64_t>(SIZE_MAX) || (size > 0 && !bytes)) {
        esdb_detail::set_error(error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_VALUE, nullptr, SQLITE_MISUSE, "invalid byte payload");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    esdb_status status = allocate_value(ESDB_VALUE_BYTES, out_value, error);
    if (status != ESDB_OK) return status;
    try {
        if (size > 0) {
            const auto *begin = reinterpret_cast<const std::uint8_t *>(bytes);
            (*out_value)->payload.assign(begin, begin + static_cast<std::size_t>(size));
        }
    } catch (...) {
        esdb_value_destroy(*out_value);
        *out_value = nullptr;
        esdb_detail::set_error(error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_VALUE, nullptr, SQLITE_NOMEM, "byte payload allocation failed");
        return ESDB_ERR_OUT_OF_MEMORY;
    }
    return ESDB_OK;
}

void esdb_value_destroy(esdb_value *value) { delete value; }
esdb_value_type esdb_value_type_of(const esdb_value *value) {
    return value ? value->type : static_cast<esdb_value_type>(ESDB_VALUE_NULL);
}

esdb_status esdb_value_get_bool(const esdb_value *value, int *out_value) {
    if (!value || !out_value) return ESDB_ERR_INVALID_ARGUMENT;
    if (value->type != ESDB_VALUE_BOOL || value->payload.size() != 1) return ESDB_ERR_TYPE_MISMATCH;
    *out_value = value->payload[0] ? 1 : 0;
    return ESDB_OK;
}

esdb_status esdb_value_get_int32(const esdb_value *value, int32_t *out_value) {
    if (!value || !out_value) return ESDB_ERR_INVALID_ARGUMENT;
    if (value->type != ESDB_VALUE_INT32 || value->payload.size() != 4) return ESDB_ERR_TYPE_MISMATCH;
    *out_value = static_cast<int32_t>(read_le32(value->payload));
    return ESDB_OK;
}

esdb_status esdb_value_get_int64(const esdb_value *value, int64_t *out_value) {
    if (!value || !out_value) return ESDB_ERR_INVALID_ARGUMENT;
    if (value->type != ESDB_VALUE_INT64 || value->payload.size() != 8) return ESDB_ERR_TYPE_MISMATCH;
    *out_value = static_cast<int64_t>(read_le64(value->payload));
    return ESDB_OK;
}

esdb_status esdb_value_get_double(const esdb_value *value, double *out_value) {
    if (!value || !out_value) return ESDB_ERR_INVALID_ARGUMENT;
    if (value->type != ESDB_VALUE_DOUBLE || value->payload.size() != 8) return ESDB_ERR_TYPE_MISMATCH;
    const std::uint64_t bits = read_le64(value->payload);
    std::memcpy(out_value, &bits, sizeof(bits));
    return ESDB_OK;
}

esdb_status esdb_value_get_data(const esdb_value *value, const void **out_data, uint64_t *out_size) {
    if (!value || !out_data || !out_size) return ESDB_ERR_INVALID_ARGUMENT;
    switch (value->type) {
        case ESDB_VALUE_UTF8:
        case ESDB_VALUE_BYTES:
        case ESDB_VALUE_ARRAY:
        case ESDB_VALUE_OBJECT:
            *out_data = value->payload.empty() ? nullptr : value->payload.data();
            *out_size = static_cast<uint64_t>(value->payload.size());
            return ESDB_OK;
        default:
            return ESDB_ERR_TYPE_MISMATCH;
    }
}
