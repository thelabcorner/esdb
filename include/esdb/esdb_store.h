#ifndef ESDB_ESDB_STORE_H
#define ESDB_ESDB_STORE_H

#include "esdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESDB Store: an optional durable object-store layer above ESDB Runtime.
 *
 * The Store is IndexedDB-shaped in semantics (named stores, opaque keys,
 * typed values, a monotonic revision, and a change log for polling) and is
 * layered above the same Runtime engine. Runtime does not depend on it:
 * consumers with their own relational schema (for example Workmark) use
 * Runtime directly and never pass through the Store.
 *
 * v0.1 ships exactly one durability tier: every mutation is transactional.
 * Outside a caller-owned transaction, success includes its SQLite commit. When
 * called inside an outer transaction, the returned revision/state is
 * provisional until that outer transaction commits. The tier vocabulary ("memory", "buffered",
 * "durable", "cache") is reserved for the documented Store roadmap; it is not
 * exposed here until a second tier actually exists.
 *
 * Store names and keys are data, never SQL identifiers; they may be any valid
 * UTF-8 string within the documented byte limits. The reserved schema prefix
 * `__esdb_` belongs to ESDB and must not be used by product tables in the same
 * database.
 */

#define ESDB_STORE_NAME_MAX_BYTES 255u
#define ESDB_STORE_KEY_MAX_BYTES 4096u
#define ESDB_CHANGE_LIMIT_MAX 10000u
#define ESDB_REVISION_MAX UINT64_C(9223372036854775807)

typedef uint32_t esdb_change_operation;
enum {
    ESDB_CHANGE_PUT = 1u,
    ESDB_CHANGE_DELETE = 2u
};

typedef struct esdb_change {
    uint64_t revision;
    esdb_change_operation operation;
    esdb_value_type value_type;
    const char *store_name;
    const char *key;
} esdb_change;

/*
 * Change callback contract:
 *   - `change` and its strings are borrowed and valid only for the duration
 *     of the call.
 *   - return 0 to continue iteration, non-zero to stop early.
 */
typedef int (*esdb_change_callback)(const esdb_change *change, void *user_data);

/*
 * Ensures the store exists. Creates the shared Store schema on first use and
 * registers the store name. Idempotent.
 */
ESDB_API esdb_status esdb_store_ensure(
    esdb_database *database,
    const char *store_name_utf8,
    esdb_error *error);

/*
 * Inserts or replaces `value` under `key`. On success *out_revision (when
 * provided) receives the revision assigned to this mutation. Inside a
 * caller-owned transaction that revision is provisional until outer commit
 * and may be reused if the outer transaction rolls back.
 */
ESDB_API esdb_status esdb_store_put(
    esdb_database *database,
    const char *store_name_utf8,
    const char *key_utf8,
    const esdb_value *value,
    uint64_t *out_revision,
    esdb_error *error);

/*
 * Reads a value. The returned value is owned by the caller and must be
 * destroyed with esdb_value_destroy(). A missing key returns
 * ESDB_ERR_NOT_FOUND.
 */
ESDB_API esdb_status esdb_store_get(
    esdb_database *database,
    const char *store_name_utf8,
    const char *key_utf8,
    esdb_value **out_value,
    esdb_error *error);

/* Deletes a key. *out_deleted reports whether a row existed. */
ESDB_API esdb_status esdb_store_delete(
    esdb_database *database,
    const char *store_name_utf8,
    const char *key_utf8,
    int *out_deleted,
    uint64_t *out_revision,
    esdb_error *error);

ESDB_API esdb_status esdb_store_exists(
    esdb_database *database,
    const char *store_name_utf8,
    const char *key_utf8,
    int *out_exists,
    esdb_error *error);

ESDB_API esdb_status esdb_store_count(
    esdb_database *database,
    const char *store_name_utf8,
    uint64_t *out_count,
    esdb_error *error);

/*
 * Current global Store revision (0 on a database that has never mutated).
 * Revisions are in the inclusive range 1..ESDB_REVISION_MAX once mutations
 * exist. The revision is durable metadata: pruning the change log does not reset it,
 * and reopening the database preserves the last committed revision.
 */
ESDB_API esdb_status esdb_store_revision(
    esdb_database *database,
    uint64_t *out_revision,
    esdb_error *error);

/*
 * Iterates change records with revision > after_revision, oldest first.
 * after_revision must be <= ESDB_REVISION_MAX.
 * `store_name_or_null` filters to one store when non-NULL. `limit` is clamped
 * to ESDB_CHANGE_LIMIT_MAX (0 means the clamp maximum).
 *
 * *out_last_revision receives the highest revision examined, and
 * *out_change_count the number of changes delivered.
 */
ESDB_API esdb_status esdb_store_changes_since(
    esdb_database *database,
    const char *store_name_or_null_utf8,
    uint64_t after_revision,
    uint32_t limit,
    esdb_change_callback callback,
    void *user_data,
    uint64_t *out_last_revision,
    uint32_t *out_change_count,
    esdb_error *error);

/*
 * Deletes change records with revision <= through_revision (which must be <=
 * ESDB_REVISION_MAX). Returns the number
 * of records removed. Pruning does not affect store contents or the durable
 * global revision. Consumers that require gap detection should retain their own
 * acknowledged revision and pruning policy; v0.1 does not synthesize missing
 * historical rows after they have been pruned.
 */
ESDB_API esdb_status esdb_store_prune_changes(
    esdb_database *database,
    uint64_t through_revision,
    uint64_t *out_deleted,
    esdb_error *error);

/*
 * Subscription: a cursor over the change log for polling consumers.
 * after_revision must be <= ESDB_REVISION_MAX. A subscription is connection-scoped and not thread-safe; poll it from the
 * same thread that owns the database handle (or under FULLMUTEX with external
 * serialization of the poll sequence).
 *
 * esdb_subscription_poll() delivers changes with revision greater than the
 * subscription's current revision and advances it to the highest revision
 * delivered. This is the polling primitive a reactive Store builds on; it
 * never invokes a callback asynchronously and never calls into a host.
 */
ESDB_API esdb_status esdb_subscribe(
    esdb_database *database,
    const char *store_name_or_null_utf8,
    uint64_t after_revision,
    esdb_subscription **out_subscription,
    esdb_error *error);

ESDB_API esdb_status esdb_subscription_poll(
    esdb_subscription *subscription,
    uint32_t limit,
    esdb_change_callback callback,
    void *user_data,
    uint32_t *out_change_count,
    esdb_error *error);

ESDB_API uint64_t esdb_subscription_revision(const esdb_subscription *subscription);
ESDB_API void esdb_subscription_destroy(esdb_subscription *subscription);

#ifdef __cplusplus
}
#endif

#endif /* ESDB_ESDB_STORE_H */
