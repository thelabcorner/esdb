#ifndef ESDB_ESDB_VALUE_H
#define ESDB_ESDB_VALUE_H

#include <stdint.h>

#include "esdb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Canonical ESDB value domain.
 *
 * Values are owned by the caller and destroyed explicitly with
 * esdb_value_destroy(). A value is never shared: esdb_value_get_data()
 * returns a pointer into the value's own storage that remains valid until the
 * value is destroyed.
 *
 * Type mapping rules (see docs/ABI.md "Value and wire mapping"):
 *
 *   ESDB_VALUE_NULL    - SQL NULL.
 *   ESDB_VALUE_BOOL    - SQL INTEGER 0/1.
 *   ESDB_VALUE_INT32   - exact signed 32-bit integer.
 *   ESDB_VALUE_INT64   - exact signed 64-bit integer. This type exists
 *                        precisely so callers never have to round a 64-bit
 *                        integer through a double.
 *   ESDB_VALUE_DOUBLE  - IEEE-754 binary64.
 *   ESDB_VALUE_UTF8    - validated UTF-8 text (no embedded NUL requirement;
 *                        length is explicit).
 *   ESDB_VALUE_BYTES   - arbitrary octets, including embedded NUL.
 *   ESDB_VALUE_ARRAY   - validated UTF-8 structured-array payload.
 *   ESDB_VALUE_OBJECT  - validated UTF-8 structured-object payload.
 *
 * ARRAY and OBJECT are semantic tags, not a built-in parser. ESDB validates
 * UTF-8 at this layer but does not claim JSON/ESON syntax validation. A caller
 * that uses ESON/RFC 8259 text must validate/encode that contract above ESDB.
 */

typedef uint32_t esdb_value_type;
enum {
    ESDB_VALUE_NULL = 0u,
    ESDB_VALUE_BOOL = 1u,
    ESDB_VALUE_INT32 = 2u,
    ESDB_VALUE_INT64 = 3u,
    ESDB_VALUE_DOUBLE = 4u,
    ESDB_VALUE_UTF8 = 5u,
    ESDB_VALUE_BYTES = 6u,
    ESDB_VALUE_ARRAY = 7u,
    ESDB_VALUE_OBJECT = 8u
};

typedef struct esdb_value esdb_value;

ESDB_API esdb_status esdb_value_create_null(esdb_value **out_value, esdb_error *error);
ESDB_API esdb_status esdb_value_create_bool(int value, esdb_value **out_value, esdb_error *error);
ESDB_API esdb_status esdb_value_create_int32(int32_t value, esdb_value **out_value, esdb_error *error);
ESDB_API esdb_status esdb_value_create_int64(int64_t value, esdb_value **out_value, esdb_error *error);
ESDB_API esdb_status esdb_value_create_double(double value, esdb_value **out_value, esdb_error *error);

/*
 * Creates a UTF8, ARRAY, or OBJECT value from an explicit-length UTF-8
 * payload. The payload is copied; the caller keeps ownership of `utf8`.
 * Invalid UTF-8 is rejected with ESDB_ERR_INVALID_ARGUMENT.
 */
ESDB_API esdb_status esdb_value_create_text(
    esdb_value_type type,
    const char *utf8,
    uint64_t size,
    esdb_value **out_value,
    esdb_error *error);

/* Copies `size` octets; embedded NUL bytes are preserved. */
ESDB_API esdb_status esdb_value_create_bytes(
    const void *bytes,
    uint64_t size,
    esdb_value **out_value,
    esdb_error *error);

ESDB_API void esdb_value_destroy(esdb_value *value);
ESDB_API esdb_value_type esdb_value_type_of(const esdb_value *value);

ESDB_API esdb_status esdb_value_get_bool(const esdb_value *value, int *out_value);
ESDB_API esdb_status esdb_value_get_int32(const esdb_value *value, int32_t *out_value);
ESDB_API esdb_status esdb_value_get_int64(const esdb_value *value, int64_t *out_value);
ESDB_API esdb_status esdb_value_get_double(const esdb_value *value, double *out_value);

/*
 * Borrows the payload of a UTF8/ARRAY/OBJECT/BYTES value. The returned
 * pointer is valid until the value is destroyed and must not be freed.
 * `out_size` excludes any terminator: payloads are length-delimited.
 */
ESDB_API esdb_status esdb_value_get_data(
    const esdb_value *value,
    const void **out_data,
    uint64_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* ESDB_ESDB_VALUE_H */
