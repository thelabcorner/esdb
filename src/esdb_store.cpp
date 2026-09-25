#include "esdb_internal.hpp"

#include <esdb/esdb_store.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct ByteLess {
    bool operator()(const std::string &left, const std::string &right) const noexcept {
        const std::size_t common = std::min(left.size(), right.size());
        for (std::size_t i = 0; i < common; ++i) {
            const unsigned char a = static_cast<unsigned char>(left[i]);
            const unsigned char b = static_cast<unsigned char>(right[i]);
            if (a < b) return true;
            if (a > b) return false;
        }
        return left.size() < right.size();
    }
};

struct MemoryRecord {
    esdb_value value;
    std::uint64_t revision = 0u;
};

struct MemoryChange {
    std::uint64_t revision = 0u;
    esdb_store_change_operation operation = ESDB_STORE_CHANGE_PUT;
    esdb_value_type value_type = ESDB_VALUE_NULL;
    std::string key;
};

struct MemoryStoreState {
    explicit MemoryStoreState(std::string store_name)
        : name(std::move(store_name)) {}

    std::string name;
    esdb_detail::NoThrowMutex mutex;
    std::map<std::string, MemoryRecord, ByteLess> records;
    std::deque<MemoryChange> changes;
    std::uint64_t revision = 0u;
    std::uint64_t retained_floor = 0u;
    std::atomic<std::uint32_t> handle_count{0u};
    std::atomic<std::uint32_t> subscription_count{0u};
};

struct OwnedRecord {
    std::string key;
    esdb_value value;
    std::uint64_t revision = 0u;
};

struct OwnedChange {
    std::uint64_t revision = 0u;
    esdb_store_change_operation operation = ESDB_STORE_CHANGE_PUT;
    esdb_value_type value_type = ESDB_VALUE_NULL;
    std::string key;
};

esdb_detail::NoThrowMutex &registry_mutex() {
    static esdb_detail::NoThrowMutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::shared_ptr<MemoryStoreState>> &registry() {
    static std::unordered_map<std::string, std::shared_ptr<MemoryStoreState>> stores;
    return stores;
}

bool valid_value_type(esdb_value_type type) noexcept {
    return type >= ESDB_VALUE_NULL && type <= ESDB_VALUE_OBJECT;
}

bool valid_name(const char *text) noexcept {
    if (!esdb_detail::valid_c_string(text, ESDB_STORE_NAME_MAX_BYTES)) return false;
    return esdb_detail::valid_utf8(text, static_cast<std::uint64_t>(std::strlen(text)));
}

bool valid_key(const char *text) noexcept {
    if (!esdb_detail::valid_c_string(text, ESDB_STORE_KEY_MAX_BYTES)) return false;
    return esdb_detail::valid_utf8(text, static_cast<std::uint64_t>(std::strlen(text)));
}

esdb_status store_fail(
    esdb_error *error,
    esdb_status status,
    esdb_phase phase,
    const char *message) noexcept {
    esdb_detail::set_error(error, status, phase, nullptr, 0, message);
    return status;
}

esdb_status clone_value(
    const esdb_value *source,
    esdb_value **out_value,
    esdb_error *error,
    esdb_phase phase) noexcept {
    if (out_value) *out_value = nullptr;
    if (!source || !out_value || !valid_value_type(source->type)) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, phase,
            "invalid Store value clone arguments");
    }

    esdb_value *copy = new (std::nothrow) esdb_value();
    if (!copy) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, phase,
            "failed to allocate Store value");
    }

    try {
        copy->type = source->type;
        copy->payload = source->payload;
    } catch (...) {
        delete copy;
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, phase,
            "failed to copy Store value payload");
    }

    *out_value = copy;
    return ESDB_OK;
}

void retain_change(MemoryStoreState &state, MemoryChange &&change) {
    state.changes.emplace_back(std::move(change));
    while (state.changes.size() > ESDB_STORE_CHANGE_CAPACITY) {
        state.retained_floor = state.changes.front().revision;
        state.changes.pop_front();
    }
}

}  // namespace

struct esdb_store {
    std::shared_ptr<MemoryStoreState> state;
};

struct esdb_store_subscription {
    std::shared_ptr<MemoryStoreState> state;
    std::atomic<std::uint64_t> revision{0u};
    esdb_detail::NoThrowMutex poll_mutex;
};

namespace {

esdb_status validate_store(
    esdb_store *store,
    esdb_error *error,
    esdb_phase phase) {
    if (!store || !store->state) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, phase,
            "Store handle is required");
    }
    return ESDB_OK;
}

esdb_status changes_since_impl(
    const std::shared_ptr<MemoryStoreState> &state,
    std::uint64_t after_revision,
    std::uint32_t limit,
    esdb_store_change_callback callback,
    void *user_data,
    std::uint64_t *out_last_revision,
    std::uint32_t *out_change_count,
    esdb_error *error) noexcept {
    esdb_detail::clear_error(error);
    if (out_last_revision) *out_last_revision = after_revision;
    if (out_change_count) *out_change_count = 0u;

    if (!state || !callback || limit > ESDB_STORE_CHANGE_LIMIT_MAX ||
        after_revision > ESDB_REVISION_MAX) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_SUBSCRIPTION,
            "invalid Store change query arguments");
    }
    if (limit == 0u) limit = ESDB_STORE_CHANGE_LIMIT_MAX;

    std::vector<OwnedChange> snapshot;
    try {
        snapshot.reserve(limit < 256u ? limit : 256u);
        {
            std::lock_guard<esdb_detail::NoThrowMutex> lock(state->mutex);
            if (after_revision > state->revision) {
                return store_fail(
                    error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_SUBSCRIPTION,
                    "after_revision is newer than the Store");
            }
            if (after_revision < state->retained_floor) {
                return store_fail(
                    error, ESDB_ERR_GAP, ESDB_PHASE_SUBSCRIPTION,
                    "requested Store history is older than the retained change floor");
            }

            for (const MemoryChange &change : state->changes) {
                if (change.revision <= after_revision) continue;
                if (snapshot.size() >= limit) break;
                OwnedChange owned;
                owned.revision = change.revision;
                owned.operation = change.operation;
                owned.value_type = change.value_type;
                owned.key = change.key;
                snapshot.emplace_back(std::move(owned));
            }
        }
    } catch (const std::bad_alloc &) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_SUBSCRIPTION,
            "failed to allocate Store change snapshot");
    } catch (...) {
        return store_fail(
            error, ESDB_ERR_INTERNAL, ESDB_PHASE_SUBSCRIPTION,
            "unexpected exception while snapshotting Store changes");
    }

    std::uint64_t last = after_revision;
    std::uint32_t delivered = 0u;
    for (const OwnedChange &owned : snapshot) {
        esdb_store_change change{};
        change.revision = owned.revision;
        change.operation = owned.operation;
        change.value_type = owned.value_type;
        change.key =
            owned.operation == ESDB_STORE_CHANGE_CLEAR
                ? nullptr
                : owned.key.c_str();

        last = change.revision;
        ++delivered;
        try {
            if (callback(&change, user_data) != 0) break;
        } catch (...) {
            return store_fail(
                error, ESDB_ERR_INTERNAL, ESDB_PHASE_SUBSCRIPTION,
                "Store change callback threw an exception");
        }
    }

    if (out_last_revision) *out_last_revision = last;
    if (out_change_count) *out_change_count = delivered;
    return ESDB_OK;
}

}  // namespace

esdb_status esdb_store_open(
    const char *name_utf8,
    esdb_store **out_store,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_store) *out_store = nullptr;
    if (!out_store || !valid_name(name_utf8)) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "Store name and out_store are required");
    }

    try {
        std::shared_ptr<MemoryStoreState> state;
        {
            std::lock_guard<esdb_detail::NoThrowMutex> lock(registry_mutex());
            auto &stores = registry();
            const auto found = stores.find(name_utf8);
            if (found != stores.end()) {
                state = found->second;
            } else {
                state = std::make_shared<MemoryStoreState>(std::string(name_utf8));
                stores.emplace(state->name, state);
            }
            state->handle_count.fetch_add(1u, std::memory_order_relaxed);
        }

        esdb_store *handle = new (std::nothrow) esdb_store();
        if (!handle) {
            state->handle_count.fetch_sub(1u, std::memory_order_relaxed);
            return store_fail(
                error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
                "failed to allocate Store handle");
        }
        handle->state = std::move(state);
        *out_store = handle;
        return ESDB_OK;
    } catch (const std::bad_alloc &) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            "failed to allocate named Store");
    } catch (...) {
        return store_fail(
            error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
            "unexpected exception while opening Store");
    }
}

void esdb_store_close(esdb_store *store) {
    if (!store) return;
    if (store->state) {
        store->state->handle_count.fetch_sub(1u, std::memory_order_relaxed);
    }
    delete store;
}

esdb_status esdb_store_destroy(
    const char *name_utf8,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (!valid_name(name_utf8)) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "valid Store name is required");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> lock(registry_mutex());
    auto &stores = registry();
    const auto found = stores.find(name_utf8);
    if (found == stores.end()) {
        return store_fail(
            error, ESDB_ERR_NOT_FOUND, ESDB_PHASE_STORE,
            "named Store does not exist");
    }

    const std::shared_ptr<MemoryStoreState> &state = found->second;
    if (state->handle_count.load(std::memory_order_relaxed) != 0u ||
        state->subscription_count.load(std::memory_order_relaxed) != 0u) {
        return store_fail(
            error, ESDB_ERR_BUSY, ESDB_PHASE_STORE,
            "named Store still has active handles or subscriptions");
    }
    stores.erase(found);
    return ESDB_OK;
}

const char *esdb_store_name(const esdb_store *store) {
    return store && store->state ? store->state->name.c_str() : nullptr;
}

esdb_status esdb_store_put(
    esdb_store *store,
    const char *key_utf8,
    const esdb_value *value,
    std::uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_revision) *out_revision = 0u;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        !valid_key(key_utf8) || !value || !valid_value_type(value->type)) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "invalid Store put arguments");
    }

    try {
        std::string map_key(key_utf8);
        MemoryRecord candidate;
        candidate.value.type = value->type;
        candidate.value.payload = value->payload;

        MemoryChange change;
        change.operation = ESDB_STORE_CHANGE_PUT;
        change.value_type = value->type;
        change.key.assign(key_utf8);

        std::shared_ptr<MemoryStoreState> state = store->state;
        std::lock_guard<esdb_detail::NoThrowMutex> lock(state->mutex);
        if (state->revision >= ESDB_REVISION_MAX) {
            return store_fail(
                error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_STORE,
                "Store revision domain is exhausted");
        }

        const std::uint64_t revision = state->revision + 1u;
        candidate.revision = revision;
        change.revision = revision;

        /*
         * Preserve the strong mutation guarantee even under allocation
         * failure: stage the record mutation first, then publish the change
         * event. If the deque allocation fails, restore the previous record
         * without allocating. retain_change() only prunes history after its
         * append has succeeded.
         */
        auto existing = state->records.find(map_key);
        bool inserted = false;
        MemoryRecord previous;
        if (existing != state->records.end()) {
            previous = std::move(existing->second);
            existing->second = std::move(candidate);
        } else {
            auto inserted_result =
                state->records.emplace(map_key, std::move(candidate));
            existing = inserted_result.first;
            inserted = inserted_result.second;
        }

        try {
            retain_change(*state, std::move(change));
        } catch (...) {
            if (inserted) {
                state->records.erase(existing);
            } else {
                existing->second = std::move(previous);
            }
            throw;
        }

        state->revision = revision;
        if (out_revision) *out_revision = revision;
        return ESDB_OK;
    } catch (const std::bad_alloc &) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            "failed to allocate Store mutation");
    } catch (...) {
        return store_fail(
            error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
            "unexpected exception during Store put");
    }
}

esdb_status esdb_store_patch(
    esdb_store *store,
    const esdb_store_patch_entry *entries,
    std::uint32_t count,
    std::uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_revision) *out_revision = 0u;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        (count != 0u && !entries) ||
        count > ESDB_STORE_PATCH_MAX_ENTRIES) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "invalid Store patch arguments");
    }

    for (std::uint32_t i = 0u; i < count; ++i) {
        if (!valid_key(entries[i].key) ||
            !entries[i].value ||
            !valid_value_type(entries[i].value->type)) {
            return store_fail(
                error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
                "Store patch contains an invalid key or value");
        }
    }

    std::shared_ptr<MemoryStoreState> state = store->state;
    std::lock_guard<esdb_detail::NoThrowMutex> lock(state->mutex);

    if (count == 0u) {
        if (out_revision) *out_revision = state->revision;
        return ESDB_OK;
    }
    if (state->revision > ESDB_REVISION_MAX - count) {
        return store_fail(
            error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_STORE,
            "Store revision domain is exhausted");
    }

    /*
     * Copy-on-write is deliberate here: single-key put() stays on the fast
     * path, while multi-key patch() pays O(n) copying to guarantee failure
     * atomicity even under allocator failure. Nothing in the live Store is
     * changed until both the candidate records and change journal are complete.
     */
    try {
        auto candidate_records = state->records;
        auto candidate_changes = state->changes;
        std::uint64_t revision = state->revision;
        std::uint64_t candidate_floor = state->retained_floor;

        for (std::uint32_t i = 0u; i < count; ++i) {
            ++revision;

            MemoryRecord record;
            record.value.type = entries[i].value->type;
            record.value.payload = entries[i].value->payload;
            record.revision = revision;
            candidate_records[entries[i].key] = std::move(record);

            MemoryChange change;
            change.revision = revision;
            change.operation = ESDB_STORE_CHANGE_PUT;
            change.value_type = entries[i].value->type;
            change.key.assign(entries[i].key);
            candidate_changes.emplace_back(std::move(change));

            while (candidate_changes.size() > ESDB_STORE_CHANGE_CAPACITY) {
                candidate_floor = candidate_changes.front().revision;
                candidate_changes.pop_front();
            }
        }

        state->records.swap(candidate_records);
        state->changes.swap(candidate_changes);
        state->retained_floor = candidate_floor;
        state->revision = revision;
        if (out_revision) *out_revision = revision;
        return ESDB_OK;
    } catch (const std::bad_alloc &) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            "failed to allocate atomic Store patch");
    } catch (...) {
        return store_fail(
            error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
            "unexpected exception during Store patch");
    }
}

esdb_status esdb_store_get(
    esdb_store *store,
    const char *key_utf8,
    esdb_value **out_value,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_value) *out_value = nullptr;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        !out_value || !valid_key(key_utf8)) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "invalid Store get arguments");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> lock(store->state->mutex);
    const auto found = store->state->records.find(key_utf8);
    if (found == store->state->records.end()) {
        return ESDB_ERR_NOT_FOUND;
    }
    return clone_value(
        &found->second.value, out_value, error, ESDB_PHASE_STORE);
}

esdb_status esdb_store_delete(
    esdb_store *store,
    const char *key_utf8,
    int *out_deleted,
    std::uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_deleted) *out_deleted = 0;
    if (out_revision) *out_revision = 0u;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        !valid_key(key_utf8)) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "invalid Store delete arguments");
    }

    try {
        MemoryChange change;
        change.operation = ESDB_STORE_CHANGE_DELETE;
        change.value_type = ESDB_VALUE_NULL;
        change.key.assign(key_utf8);

        std::shared_ptr<MemoryStoreState> state = store->state;
        std::lock_guard<esdb_detail::NoThrowMutex> lock(state->mutex);
        const auto found = state->records.find(key_utf8);
        if (found == state->records.end()) {
            if (out_revision) *out_revision = state->revision;
            return ESDB_OK;
        }
        if (state->revision >= ESDB_REVISION_MAX) {
            return store_fail(
                error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_STORE,
                "Store revision domain is exhausted");
        }

        const std::uint64_t revision = state->revision + 1u;
        change.revision = revision;
        retain_change(*state, std::move(change));
        state->records.erase(found);
        state->revision = revision;

        if (out_deleted) *out_deleted = 1;
        if (out_revision) *out_revision = revision;
        return ESDB_OK;
    } catch (const std::bad_alloc &) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            "failed to allocate Store delete change");
    } catch (...) {
        return store_fail(
            error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
            "unexpected exception during Store delete");
    }
}

esdb_status esdb_store_exists(
    esdb_store *store,
    const char *key_utf8,
    int *out_exists,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_exists) *out_exists = 0;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        !out_exists || !valid_key(key_utf8)) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "invalid Store exists arguments");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> lock(store->state->mutex);
    *out_exists = store->state->records.find(key_utf8) !=
            store->state->records.end()
        ? 1
        : 0;
    return ESDB_OK;
}

esdb_status esdb_store_count(
    esdb_store *store,
    std::uint64_t *out_count,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_count) *out_count = 0u;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        !out_count) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "Store and out_count are required");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> lock(store->state->mutex);
    *out_count = static_cast<std::uint64_t>(store->state->records.size());
    return ESDB_OK;
}

esdb_status esdb_store_clear(
    esdb_store *store,
    int *out_cleared,
    std::uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_cleared) *out_cleared = 0;
    if (out_revision) *out_revision = 0u;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK) {
        return ESDB_ERR_INVALID_ARGUMENT;
    }

    try {
        MemoryChange change;
        change.operation = ESDB_STORE_CHANGE_CLEAR;
        change.value_type = ESDB_VALUE_NULL;

        std::shared_ptr<MemoryStoreState> state = store->state;
        std::lock_guard<esdb_detail::NoThrowMutex> lock(state->mutex);
        if (state->records.empty()) {
            if (out_revision) *out_revision = state->revision;
            return ESDB_OK;
        }
        if (state->revision >= ESDB_REVISION_MAX) {
            return store_fail(
                error, ESDB_ERR_INVALID_STATE, ESDB_PHASE_STORE,
                "Store revision domain is exhausted");
        }

        const std::uint64_t revision = state->revision + 1u;
        change.revision = revision;
        retain_change(*state, std::move(change));
        state->records.clear();
        state->revision = revision;

        if (out_cleared) *out_cleared = 1;
        if (out_revision) *out_revision = revision;
        return ESDB_OK;
    } catch (const std::bad_alloc &) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            "failed to allocate Store clear change");
    } catch (...) {
        return store_fail(
            error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
            "unexpected exception during Store clear");
    }
}

esdb_status esdb_store_scan(
    esdb_store *store,
    const char *after_key_or_null_utf8,
    std::uint32_t limit,
    esdb_store_record_callback callback,
    void *user_data,
    std::uint32_t *out_record_count,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_record_count) *out_record_count = 0u;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        !callback ||
        (after_key_or_null_utf8 && !valid_key(after_key_or_null_utf8)) ||
        limit > ESDB_STORE_SCAN_LIMIT_MAX) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "invalid Store scan arguments");
    }
    if (limit == 0u) limit = ESDB_STORE_SCAN_LIMIT_MAX;

    std::vector<OwnedRecord> snapshot;
    try {
        snapshot.reserve(limit < 256u ? limit : 256u);
        {
            std::lock_guard<esdb_detail::NoThrowMutex> lock(store->state->mutex);
            auto current = after_key_or_null_utf8
                ? store->state->records.upper_bound(after_key_or_null_utf8)
                : store->state->records.begin();
            for (; current != store->state->records.end() &&
                   snapshot.size() < limit;
                 ++current) {
                OwnedRecord owned;
                owned.key = current->first;
                owned.value.type = current->second.value.type;
                owned.value.payload = current->second.value.payload;
                owned.revision = current->second.revision;
                snapshot.emplace_back(std::move(owned));
            }
        }
    } catch (const std::bad_alloc &) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_STORE,
            "failed to allocate Store scan snapshot");
    } catch (...) {
        return store_fail(
            error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
            "unexpected exception while snapshotting Store");
    }

    std::uint32_t delivered = 0u;
    for (OwnedRecord &owned : snapshot) {
        esdb_store_record record{};
        record.key = owned.key.c_str();
        record.value = &owned.value;
        record.revision = owned.revision;
        ++delivered;
        try {
            if (callback(&record, user_data) != 0) break;
        } catch (...) {
            return store_fail(
                error, ESDB_ERR_INTERNAL, ESDB_PHASE_STORE,
                "Store scan callback threw an exception");
        }
    }

    if (out_record_count) *out_record_count = delivered;
    return ESDB_OK;
}

esdb_status esdb_store_revision(
    esdb_store *store,
    std::uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_revision) *out_revision = 0u;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        !out_revision) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "Store and out_revision are required");
    }

    std::lock_guard<esdb_detail::NoThrowMutex> lock(store->state->mutex);
    *out_revision = store->state->revision;
    return ESDB_OK;
}

esdb_status esdb_store_changes_since(
    esdb_store *store,
    std::uint64_t after_revision,
    std::uint32_t limit,
    esdb_store_change_callback callback,
    void *user_data,
    std::uint64_t *out_last_revision,
    std::uint32_t *out_change_count,
    esdb_error *error) {
    if (validate_store(store, error, ESDB_PHASE_SUBSCRIPTION) != ESDB_OK) {
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    return changes_since_impl(
        store->state,
        after_revision,
        limit,
        callback,
        user_data,
        out_last_revision,
        out_change_count,
        error);
}

esdb_status esdb_store_retained_floor(
    esdb_store *store,
    std::uint64_t *out_revision,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_revision) *out_revision = 0u;
    if (validate_store(store, error, ESDB_PHASE_STORE) != ESDB_OK ||
        !out_revision) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_STORE,
            "Store and out_revision are required");
    }
    std::lock_guard<esdb_detail::NoThrowMutex> lock(store->state->mutex);
    *out_revision = store->state->retained_floor;
    return ESDB_OK;
}

esdb_status esdb_store_subscribe(
    esdb_store *store,
    std::uint64_t after_revision,
    esdb_store_subscription **out_subscription,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_subscription) *out_subscription = nullptr;
    if (validate_store(store, error, ESDB_PHASE_SUBSCRIPTION) != ESDB_OK ||
        !out_subscription || after_revision > ESDB_REVISION_MAX) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_SUBSCRIPTION,
            "invalid Store subscription arguments");
    }

    {
        std::lock_guard<esdb_detail::NoThrowMutex> lock(store->state->mutex);
        if (after_revision > store->state->revision) {
            return store_fail(
                error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_SUBSCRIPTION,
                "subscription revision is newer than the Store");
        }
        if (after_revision < store->state->retained_floor) {
            return store_fail(
                error, ESDB_ERR_GAP, ESDB_PHASE_SUBSCRIPTION,
                "subscription revision is older than retained Store history");
        }
    }

    esdb_store_subscription *subscription =
        new (std::nothrow) esdb_store_subscription();
    if (!subscription) {
        return store_fail(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_SUBSCRIPTION,
            "failed to allocate Store subscription");
    }

    subscription->state = store->state;
    subscription->revision.store(after_revision, std::memory_order_relaxed);
    subscription->state->subscription_count.fetch_add(
        1u, std::memory_order_relaxed);
    *out_subscription = subscription;
    return ESDB_OK;
}

esdb_status esdb_store_subscription_poll(
    esdb_store_subscription *subscription,
    std::uint32_t limit,
    esdb_store_change_callback callback,
    void *user_data,
    std::uint32_t *out_change_count,
    esdb_error *error) {
    esdb_detail::clear_error(error);
    if (out_change_count) *out_change_count = 0u;
    if (!subscription || !subscription->state || !callback) {
        return store_fail(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_SUBSCRIPTION,
            "invalid Store subscription poll arguments");
    }

    std::unique_lock<esdb_detail::NoThrowMutex> poll_lock(
        subscription->poll_mutex, std::try_to_lock);
    if (!poll_lock.owns_lock()) {
        return store_fail(
            error, ESDB_ERR_BUSY, ESDB_PHASE_SUBSCRIPTION,
            "Store subscription is already being polled");
    }

    const std::uint64_t start =
        subscription->revision.load(std::memory_order_relaxed);
    std::uint64_t last = start;
    std::uint32_t count = 0u;
    const esdb_status status = changes_since_impl(
        subscription->state,
        start,
        limit,
        callback,
        user_data,
        &last,
        &count,
        error);
    if (status == ESDB_OK) {
        subscription->revision.store(last, std::memory_order_relaxed);
    }
    if (out_change_count) *out_change_count = count;
    return status;
}

std::uint64_t esdb_store_subscription_revision(
    const esdb_store_subscription *subscription) {
    return subscription
        ? subscription->revision.load(std::memory_order_relaxed)
        : 0u;
}

void esdb_store_subscription_destroy(
    esdb_store_subscription *subscription) {
    if (!subscription) return;
    if (subscription->state) {
        subscription->state->subscription_count.fetch_sub(
            1u, std::memory_order_relaxed);
    }
    delete subscription;
}
