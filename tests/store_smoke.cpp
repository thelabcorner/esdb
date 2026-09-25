#include <esdb/esdb_store.h>
#include <esdb/esdb_value.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n",             \
                         #expr, __FILE__, __LINE__);                          \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

struct ChangeCapture {
    std::uint32_t count = 0u;
    std::uint64_t last = 0u;
};

int capture_change(const esdb_store_change *change, void *user) {
    auto *capture = static_cast<ChangeCapture *>(user);
    CHECK(change != nullptr);
    if (!change) return 1;
    ++capture->count;
    capture->last = change->revision;
    return 0;
}

struct Reentrant {
    esdb_store_subscription *subscription = nullptr;
    esdb_status nested = ESDB_OK;
};

int reentrant_change(const esdb_store_change *, void *user) {
    auto *context = static_cast<Reentrant *>(user);
    ChangeCapture nested_capture{};
    std::uint32_t delivered = 0u;
    esdb_error error{};
    context->nested = esdb_store_subscription_poll(
        context->subscription,
        1u,
        capture_change,
        &nested_capture,
        &delivered,
        &error);
    CHECK(context->nested == ESDB_ERR_BUSY);
    CHECK(error.status == ESDB_ERR_BUSY);
    return 1;
}

}  // namespace

int main() {
    const char *name = "esdb.test.memory-store";
    esdb_error error{};

    /* Cleanup from an interrupted prior run is allowed to report not-found. */
    const esdb_status pre_destroy = esdb_store_destroy(name, &error);
    CHECK(pre_destroy == ESDB_OK || pre_destroy == ESDB_ERR_NOT_FOUND);

    esdb_store *first = nullptr;
    esdb_store *second = nullptr;
    CHECK(esdb_store_open(name, &first, &error) == ESDB_OK);
    CHECK(esdb_store_open(name, &second, &error) == ESDB_OK);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK(std::strcmp(esdb_store_name(first), name) == 0);

    std::uint64_t revision = 0u;
    CHECK(esdb_store_revision(first, &revision, &error) == ESDB_OK);
    CHECK(revision == 0u);

    esdb_value *value = nullptr;
    CHECK(esdb_value_create_int64(
        INT64_C(9007199254740993), &value, &error) == ESDB_OK);
    CHECK(esdb_store_put(first, "counter", value, &revision, &error) == ESDB_OK);
    CHECK(revision == 1u);
    esdb_value_destroy(value);
    value = nullptr;

    /* A second handle observes the same process-memory state immediately. */
    CHECK(esdb_store_get(second, "counter", &value, &error) == ESDB_OK);
    std::int64_t exact = 0;
    CHECK(esdb_value_get_int64(value, &exact) == ESDB_OK);
    CHECK(exact == INT64_C(9007199254740993));
    esdb_value_destroy(value);
    value = nullptr;

    int exists = 0;
    CHECK(esdb_store_exists(second, "counter", &exists, &error) == ESDB_OK);
    CHECK(exists == 1);

    std::uint64_t count = 0u;
    CHECK(esdb_store_count(second, &count, &error) == ESDB_OK);
    CHECK(count == 1u);

    /* Zustand-like patch is one atomic state transition at the visibility level. */
    esdb_value *patch_a = nullptr;
    esdb_value *patch_b = nullptr;
    CHECK(esdb_value_create_int32(11, &patch_a, &error) == ESDB_OK);
    CHECK(esdb_value_create_int32(22, &patch_b, &error) == ESDB_OK);
    esdb_store_patch_entry patch_entries[2] = {
        {"alpha", patch_a},
        {"beta", patch_b}
    };
    const std::uint64_t before_patch = revision;
    CHECK(esdb_store_patch(
        first, patch_entries, 2u, &revision, &error) == ESDB_OK);
    CHECK(revision == before_patch + 2u);
    esdb_value_destroy(patch_a);
    esdb_value_destroy(patch_b);
    patch_a = nullptr;
    patch_b = nullptr;

    CHECK(esdb_store_get(second, "alpha", &value, &error) == ESDB_OK);
    std::int32_t patch_value = 0;
    CHECK(esdb_value_get_int32(value, &patch_value) == ESDB_OK);
    CHECK(patch_value == 11);
    esdb_value_destroy(value);
    value = nullptr;
    CHECK(esdb_store_get(second, "beta", &value, &error) == ESDB_OK);
    CHECK(esdb_value_get_int32(value, &patch_value) == ESDB_OK);
    CHECK(patch_value == 22);
    esdb_value_destroy(value);
    value = nullptr;

    /* Named Store state cannot be destroyed while either surface holds it. */
    CHECK(esdb_store_destroy(name, &error) == ESDB_ERR_BUSY);

    esdb_store_subscription *subscription = nullptr;
    CHECK(esdb_store_subscribe(first, revision, &subscription, &error) == ESDB_OK);

    CHECK(esdb_value_create_bool(1, &value, &error) == ESDB_OK);
    CHECK(esdb_store_put(second, "enabled", value, &revision, &error) == ESDB_OK);
    esdb_value_destroy(value);
    value = nullptr;

    ChangeCapture capture{};
    std::uint32_t delivered = 0u;
    CHECK(esdb_store_subscription_poll(
        subscription,
        16u,
        capture_change,
        &capture,
        &delivered,
        &error) == ESDB_OK);
    CHECK(delivered == 1u);
    CHECK(capture.count == 1u);
    CHECK(capture.last == revision);
    CHECK(esdb_store_subscription_revision(subscription) == revision);

    /* Same-subscription reentrancy is deterministic BUSY, not deadlock. */
    CHECK(esdb_value_create_int32(7, &value, &error) == ESDB_OK);
    CHECK(esdb_store_put(first, "reentrant", value, &revision, &error) == ESDB_OK);
    esdb_value_destroy(value);
    value = nullptr;

    Reentrant reentrant{};
    reentrant.subscription = subscription;
    delivered = 0u;
    CHECK(esdb_store_subscription_poll(
        subscription,
        1u,
        reentrant_change,
        &reentrant,
        &delivered,
        &error) == ESDB_OK);
    CHECK(delivered == 1u);
    CHECK(reentrant.nested == ESDB_ERR_BUSY);

    esdb_store_subscription_destroy(subscription);
    subscription = nullptr;

    /* Concurrent handles mutate one shared state/revision sequence. */
    constexpr int thread_count = 4;
    constexpr int writes_per_thread = 64;
    std::atomic<int> write_errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t]() {
            esdb_store *local = nullptr;
            esdb_error local_error{};
            if (esdb_store_open(name, &local, &local_error) != ESDB_OK) {
                write_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            esdb_value *local_value = nullptr;
            if (esdb_value_create_int32(t, &local_value, &local_error) != ESDB_OK) {
                write_errors.fetch_add(1, std::memory_order_relaxed);
                esdb_store_close(local);
                return;
            }
            for (int i = 0; i < writes_per_thread; ++i) {
                char key[64]{};
                std::snprintf(key, sizeof(key), "thread-%d-%03d", t, i);
                if (esdb_store_put(
                        local, key, local_value, nullptr, &local_error) != ESDB_OK) {
                    write_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
            esdb_value_destroy(local_value);
            esdb_store_close(local);
        });
    }
    for (auto &thread : threads) thread.join();
    CHECK(write_errors.load(std::memory_order_relaxed) == 0);

    CHECK(esdb_store_count(first, &count, &error) == ESDB_OK);
    CHECK(count == static_cast<std::uint64_t>(
        5 + thread_count * writes_per_thread));

    /* Force the bounded change ring to advance and verify explicit gap report. */
    CHECK(esdb_value_create_int32(1, &value, &error) == ESDB_OK);
    for (std::uint32_t i = 0u; i < ESDB_STORE_CHANGE_CAPACITY + 8u; ++i) {
        char key[64]{};
        std::snprintf(key, sizeof(key), "gap-%05u", i);
        CHECK(esdb_store_put(first, key, value, nullptr, &error) == ESDB_OK);
    }
    esdb_value_destroy(value);
    value = nullptr;

    std::uint64_t floor = 0u;
    CHECK(esdb_store_retained_floor(first, &floor, &error) == ESDB_OK);
    CHECK(floor > 0u);

    ChangeCapture stale{};
    std::uint64_t last = 0u;
    delivered = 0u;
    CHECK(esdb_store_changes_since(
        first,
        0u,
        1u,
        capture_change,
        &stale,
        &last,
        &delivered,
        &error) == ESDB_ERR_GAP);
    CHECK(error.status == ESDB_ERR_GAP);

    int cleared = 0;
    CHECK(esdb_store_clear(first, &cleared, &revision, &error) == ESDB_OK);
    CHECK(cleared == 1);
    CHECK(esdb_store_count(second, &count, &error) == ESDB_OK);
    CHECK(count == 0u);

    esdb_store_close(second);
    esdb_store_close(first);
    first = nullptr;
    second = nullptr;

    CHECK(esdb_store_destroy(name, &error) == ESDB_OK);

    /* Explicit destroy is the process-memory lifecycle reset. */
    CHECK(esdb_store_open(name, &first, &error) == ESDB_OK);
    CHECK(esdb_store_revision(first, &revision, &error) == ESDB_OK);
    CHECK(revision == 0u);
    CHECK(esdb_store_count(first, &count, &error) == ESDB_OK);
    CHECK(count == 0u);
    esdb_store_close(first);
    CHECK(esdb_store_destroy(name, &error) == ESDB_OK);

    if (failures != 0) {
        std::fprintf(stderr, "%d memory Store checks failed\n", failures);
        return 1;
    }
    return 0;
}
