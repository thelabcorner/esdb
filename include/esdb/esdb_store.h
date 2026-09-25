#ifndef ESDB_ESDB_STORE_H
#define ESDB_ESDB_STORE_H

#include "esdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESDB Store: process-memory application state.
 *
 * A Store is deliberately NOT a SQLite database. It is the Zustand-shaped
 * state plane of ESDB: named, typed, synchronous, revisioned, and shared by
 * every client that resolves the same ESDB core module in one process.
 *
 * Store state disappears when the hosting process exits or when the named
 * store is explicitly destroyed. Persistence is opt-in through ESDB Runtime /
 * ObjectStore bridging; no Store mutation performs disk I/O by default.
 *
 * For Adobe hosts, native plug-ins and the ExternalObject adapter share the
 * same Store only when both link/load the same shared ESDB core library.
 * Static copies intentionally have independent registries.
 */

#define ESDB_STORE_NAME_MAX_BYTES 511u
#define ESDB_STORE_KEY_MAX_BYTES 4096u
#define ESDB_STORE_CHANGE_LIMIT_MAX 10000u
#define ESDB_STORE_SCAN_LIMIT_MAX 10000u
#define ESDB_STORE_CHANGE_CAPACITY 4096u
#define ESDB_STORE_PATCH_MAX_ENTRIES 1024u

typedef struct esdb_store esdb_store;
typedef struct esdb_store_subscription esdb_store_subscription;

typedef uint32_t esdb_store_change_operation;
enum {
    ESDB_STORE_CHANGE_PUT = 1u,
    ESDB_STORE_CHANGE_DELETE = 2u,
    ESDB_STORE_CHANGE_CLEAR = 3u
};

typedef struct esdb_store_change {
    uint64_t revision;
    esdb_store_change_operation operation;
    esdb_value_type value_type;
    const char *key;
} esdb_store_change;

typedef int (*esdb_store_change_callback)(
    const esdb_store_change *change,
    void *user_data);

typedef struct esdb_store_record {
    const char *key;
    const esdb_value *value;
    uint64_t revision;
} esdb_store_record;

typedef struct esdb_store_patch_entry {
    const char *key;
    const esdb_value *value;
} esdb_store_patch_entry;

typedef int (*esdb_store_record_callback)(
    const esdb_store_record *record,
    void *user_data);

ESDB_API esdb_status esdb_store_open(
    const char *name_utf8,
    esdb_store **out_store,
    esdb_error *error);

ESDB_API void esdb_store_close(esdb_store *store);

ESDB_API esdb_status esdb_store_destroy(
    const char *name_utf8,
    esdb_error *error);

ESDB_API const char *esdb_store_name(const esdb_store *store);

ESDB_API esdb_status esdb_store_put(
    esdb_store *store,
    const char *key_utf8,
    const esdb_value *value,
    uint64_t *out_revision,
    esdb_error *error);

/*
 * Atomically applies all assignments under one Store lock. The Store is never
 * observable in a partially patched state. Each entry advances the revision;
 * out_revision receives the final revision. count==0 is a successful no-op.
 */
ESDB_API esdb_status esdb_store_patch(
    esdb_store *store,
    const esdb_store_patch_entry *entries,
    uint32_t count,
    uint64_t *out_revision,
    esdb_error *error);

ESDB_API esdb_status esdb_store_get(
    esdb_store *store,
    const char *key_utf8,
    esdb_value **out_value,
    esdb_error *error);

ESDB_API esdb_status esdb_store_delete(
    esdb_store *store,
    const char *key_utf8,
    int *out_deleted,
    uint64_t *out_revision,
    esdb_error *error);

ESDB_API esdb_status esdb_store_exists(
    esdb_store *store,
    const char *key_utf8,
    int *out_exists,
    esdb_error *error);

ESDB_API esdb_status esdb_store_count(
    esdb_store *store,
    uint64_t *out_count,
    esdb_error *error);

ESDB_API esdb_status esdb_store_clear(
    esdb_store *store,
    int *out_cleared,
    uint64_t *out_revision,
    esdb_error *error);

ESDB_API esdb_status esdb_store_scan(
    esdb_store *store,
    const char *after_key_or_null_utf8,
    uint32_t limit,
    esdb_store_record_callback callback,
    void *user_data,
    uint32_t *out_record_count,
    esdb_error *error);

ESDB_API esdb_status esdb_store_revision(
    esdb_store *store,
    uint64_t *out_revision,
    esdb_error *error);

ESDB_API esdb_status esdb_store_changes_since(
    esdb_store *store,
    uint64_t after_revision,
    uint32_t limit,
    esdb_store_change_callback callback,
    void *user_data,
    uint64_t *out_last_revision,
    uint32_t *out_change_count,
    esdb_error *error);

ESDB_API esdb_status esdb_store_retained_floor(
    esdb_store *store,
    uint64_t *out_revision,
    esdb_error *error);

ESDB_API esdb_status esdb_store_subscribe(
    esdb_store *store,
    uint64_t after_revision,
    esdb_store_subscription **out_subscription,
    esdb_error *error);

ESDB_API esdb_status esdb_store_subscription_poll(
    esdb_store_subscription *subscription,
    uint32_t limit,
    esdb_store_change_callback callback,
    void *user_data,
    uint32_t *out_change_count,
    esdb_error *error);

ESDB_API uint64_t esdb_store_subscription_revision(
    const esdb_store_subscription *subscription);

ESDB_API void esdb_store_subscription_destroy(
    esdb_store_subscription *subscription);

#ifdef __cplusplus
}
#endif

#endif /* ESDB_ESDB_STORE_H */
