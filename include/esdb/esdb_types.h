#ifndef ESDB_ESDB_TYPES_H
#define ESDB_ESDB_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(ESDB_BUILD_DLL)
#define ESDB_API __declspec(dllexport)
#elif defined(_WIN32) && defined(ESDB_USE_DLL)
#define ESDB_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define ESDB_API __attribute__((visibility("default")))
#else
#define ESDB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ESDB_VERSION_MAJOR 0u
#define ESDB_VERSION_MINOR 1u
#define ESDB_VERSION_PATCH 0u
#define ESDB_VERSION_STRING "0.1.0"

/*
 * ESDB_ABI_VERSION tracks the binary contract, not the product version. It is
 * incremented when the layout of a public struct, the meaning of a public
 * enum value, or the signature of a public function changes.
 */
#define ESDB_ABI_VERSION 1u

#define ESDB_ERROR_MESSAGE_CAPACITY 256u
#define ESDB_BACKEND_ID_CAPACITY 32u
#define ESDB_BACKEND_VERSION_CAPACITY 32u
#define ESDB_CODEC_ID_CAPACITY 32u
#define ESDB_SAVEPOINT_NAME_MAX_BYTES 64u

/*
 * Deterministic status model. Every public function returns one of these
 * values; when an esdb_error out-parameter is supplied it is filled with the
 * same status plus phase, SQLite primary/extended result codes, and a message.
 *
 * SQLite codes are preserved verbatim: ESDB_ERR_SQLITE is the fallback
 * classification, but esdb_error.sqlite_code always carries the authoritative
 * primary code (and sqlite_extended_code the extended code) so callers can
 * branch on exact SQLite semantics without ESDB reinterpreting them.
 */
typedef uint32_t esdb_status;
enum {
    ESDB_OK = 0u,
    ESDB_ERR_INVALID_ARGUMENT = 1u,
    ESDB_ERR_OUT_OF_MEMORY = 2u,
    ESDB_ERR_IO = 3u,
    ESDB_ERR_SQLITE = 4u,
    ESDB_ERR_BUSY = 5u,
    ESDB_ERR_NOT_FOUND = 6u,
    ESDB_ERR_TYPE_MISMATCH = 7u,
    ESDB_ERR_BUFFER_TOO_SMALL = 8u,
    ESDB_ERR_CONSTRAINT = 9u,
    ESDB_ERR_CORRUPT = 10u,
    ESDB_ERR_UNSUPPORTED = 11u,
    ESDB_ERR_INVALID_STATE = 12u,
    ESDB_ERR_MIGRATION = 13u,
    ESDB_ERR_INTERNAL = 14u
};

/* Operation phase, for diagnostics and deterministic error reporting. */
typedef uint32_t esdb_phase;
enum {
    ESDB_PHASE_NONE = 0u,
    ESDB_PHASE_OPEN = 1u,
    ESDB_PHASE_CONFIGURE = 2u,
    ESDB_PHASE_EXEC = 3u,
    ESDB_PHASE_TRANSACTION = 4u,
    ESDB_PHASE_SAVEPOINT = 5u,
    ESDB_PHASE_MIGRATION = 6u,
    ESDB_PHASE_INTEGRITY = 7u,
    ESDB_PHASE_BACKUP = 8u,
    ESDB_PHASE_VALUE = 9u,
    ESDB_PHASE_STORE = 10u,
    ESDB_PHASE_SUBSCRIPTION = 11u,
    ESDB_PHASE_HEALTH = 12u
};

typedef struct esdb_error {
    esdb_status status;
    esdb_phase phase;
    int32_t sqlite_code;
    int32_t sqlite_extended_code;
    char message[ESDB_ERROR_MESSAGE_CAPACITY];
} esdb_error;

#if defined(__cplusplus)
static_assert(sizeof(esdb_status) == 4u, "esdb_status ABI width");
static_assert(sizeof(esdb_phase) == 4u, "esdb_phase ABI width");
static_assert(sizeof(esdb_error) == 272u, "esdb_error ABI size");
static_assert(offsetof(esdb_error, status) == 0u, "esdb_error.status ABI offset");
static_assert(offsetof(esdb_error, phase) == 4u, "esdb_error.phase ABI offset");
static_assert(offsetof(esdb_error, sqlite_code) == 8u, "esdb_error.sqlite_code ABI offset");
static_assert(offsetof(esdb_error, sqlite_extended_code) == 12u, "esdb_error.sqlite_extended_code ABI offset");
static_assert(offsetof(esdb_error, message) == 16u, "esdb_error.message ABI offset");
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(esdb_status) == 4u, "esdb_status ABI width");
_Static_assert(sizeof(esdb_phase) == 4u, "esdb_phase ABI width");
_Static_assert(sizeof(esdb_error) == 272u, "esdb_error ABI size");
_Static_assert(offsetof(esdb_error, status) == 0u, "esdb_error.status ABI offset");
_Static_assert(offsetof(esdb_error, phase) == 4u, "esdb_error.phase ABI offset");
_Static_assert(offsetof(esdb_error, sqlite_code) == 8u, "esdb_error.sqlite_code ABI offset");
_Static_assert(offsetof(esdb_error, sqlite_extended_code) == 12u, "esdb_error.sqlite_extended_code ABI offset");
_Static_assert(offsetof(esdb_error, message) == 16u, "esdb_error.message ABI offset");
#endif

typedef struct esdb_database esdb_database;
typedef struct esdb_transaction esdb_transaction;
typedef struct esdb_subscription esdb_subscription;

ESDB_API uint32_t esdb_abi_version(void);
ESDB_API const char *esdb_version(void);
ESDB_API const char *esdb_sqlite_version(void);
ESDB_API const char *esdb_status_name(esdb_status status);
ESDB_API const char *esdb_phase_name(esdb_phase phase);
ESDB_API void esdb_error_clear(esdb_error *error);

#ifdef __cplusplus
}
#endif

#endif /* ESDB_ESDB_TYPES_H */
