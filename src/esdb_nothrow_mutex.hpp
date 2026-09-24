#ifndef ESDB_NOTHROW_MUTEX_HPP
#define ESDB_NOTHROW_MUTEX_HPP

#include <exception>
#include <mutex>

namespace esdb_detail {

/*
 * std::mutex::lock/try_lock are permitted to throw std::system_error.
 * ESDB's native ABI must never propagate a C++ exception into a C caller or
 * Adobe host, so internal lock failures are fail-fast rather than throwable.
 *
 * A lock primitive failure means the process synchronization invariant itself
 * is no longer trustworthy; there is no useful ESDB status recovery path.
 */
class NoThrowMutex final {
public:
    NoThrowMutex() noexcept = default;
    ~NoThrowMutex() noexcept = default;

    NoThrowMutex(const NoThrowMutex &) = delete;
    NoThrowMutex &operator=(const NoThrowMutex &) = delete;

    void lock() noexcept {
        try {
            mutex_.lock();
        } catch (...) {
            std::terminate();
        }
    }

    bool try_lock() noexcept {
        try {
            return mutex_.try_lock();
        } catch (...) {
            std::terminate();
        }
    }

    void unlock() noexcept {
        try {
            mutex_.unlock();
        } catch (...) {
            std::terminate();
        }
    }

private:
    std::mutex mutex_;
};

}  // namespace esdb_detail

#endif
