#include "esdb_internal.hpp"

#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>

#if defined(ESDB_HAS_ZIPVFS)
#include <zipvfs.h>
#include <zlib.h>
#if defined(ESDB_HAS_ZSTD)
#include <zstd.h>
#endif
#endif

namespace esdb_detail {

#if defined(ESDB_HAS_ZIPVFS)

namespace {

struct CodecConfig {
    esdb_codec codec = ESDB_CODEC_NONE;
    int level = 0;
    std::string vfs_name;
    const char *header = nullptr;
};

NoThrowMutex &zipvfs_registry_mutex() {
    static NoThrowMutex mutex;
    return mutex;
}

std::map<std::pair<std::uint32_t, int>, std::unique_ptr<CodecConfig>> &
zipvfs_registry() {
    static std::map<
        std::pair<std::uint32_t, int>,
        std::unique_ptr<CodecConfig>> registry;
    return registry;
}

int noop_close(void *) {
    return SQLITE_OK;
}

int zlib_bound(void *, int source_size) {
    if (source_size < 0) return 0;
    const uLong bound = compressBound(static_cast<uLong>(source_size));
    return bound > static_cast<uLong>(INT_MAX)
        ? INT_MAX
        : static_cast<int>(bound);
}

int zlib_compress(
    void *context,
    char *destination,
    int *destination_size,
    const char *source,
    int source_size) {
    if (!context || !destination || !destination_size ||
        !source || source_size < 0 || *destination_size < 0) {
        return SQLITE_MISUSE;
    }
    const auto *config = static_cast<const CodecConfig *>(context);
    uLongf size = static_cast<uLongf>(*destination_size);
    const int rc = compress2(
        reinterpret_cast<Bytef *>(destination),
        &size,
        reinterpret_cast<const Bytef *>(source),
        static_cast<uLong>(source_size),
        config->level);
    if (rc != Z_OK || size > static_cast<uLongf>(INT_MAX)) {
        return SQLITE_ERROR;
    }
    *destination_size = static_cast<int>(size);
    return SQLITE_OK;
}

int zlib_uncompress(
    void *,
    char *destination,
    int *destination_size,
    const char *source,
    int source_size) {
    if (!destination || !destination_size ||
        !source || source_size < 0 || *destination_size < 0) {
        return SQLITE_MISUSE;
    }
    uLongf size = static_cast<uLongf>(*destination_size);
    const int rc = uncompress(
        reinterpret_cast<Bytef *>(destination),
        &size,
        reinterpret_cast<const Bytef *>(source),
        static_cast<uLong>(source_size));
    if (rc != Z_OK || size > static_cast<uLongf>(INT_MAX)) {
        return SQLITE_ERROR;
    }
    *destination_size = static_cast<int>(size);
    return SQLITE_OK;
}

#if defined(ESDB_HAS_ZSTD)
int zstd_bound(void *, int source_size) {
    if (source_size < 0) return 0;
    const std::size_t bound =
        ZSTD_compressBound(static_cast<std::size_t>(source_size));
    return bound > static_cast<std::size_t>(INT_MAX)
        ? INT_MAX
        : static_cast<int>(bound);
}

int zstd_compress(
    void *context,
    char *destination,
    int *destination_size,
    const char *source,
    int source_size) {
    if (!context || !destination || !destination_size ||
        !source || source_size < 0 || *destination_size < 0) {
        return SQLITE_MISUSE;
    }
    const auto *config = static_cast<const CodecConfig *>(context);
    const std::size_t result = ZSTD_compress(
        destination,
        static_cast<std::size_t>(*destination_size),
        source,
        static_cast<std::size_t>(source_size),
        config->level);
    if (ZSTD_isError(result) ||
        result > static_cast<std::size_t>(INT_MAX)) {
        return SQLITE_ERROR;
    }
    *destination_size = static_cast<int>(result);
    return SQLITE_OK;
}

int zstd_uncompress(
    void *,
    char *destination,
    int *destination_size,
    const char *source,
    int source_size) {
    if (!destination || !destination_size ||
        !source || source_size < 0 || *destination_size < 0) {
        return SQLITE_MISUSE;
    }
    const std::size_t result = ZSTD_decompress(
        destination,
        static_cast<std::size_t>(*destination_size),
        source,
        static_cast<std::size_t>(source_size));
    if (ZSTD_isError(result) ||
        result > static_cast<std::size_t>(INT_MAX)) {
        return SQLITE_ERROR;
    }
    *destination_size = static_cast<int>(result);
    return SQLITE_OK;
}
#endif

int zipvfs_autodetect(
    void *context,
    const char *,
    const char *header,
    ZipvfsMethods *methods) {
    if (!context || !methods) return SQLITE_MISUSE;
    auto *config = static_cast<CodecConfig *>(context);

    /*
     * This VFS is selected only for an explicit ESDB compressed open. A
     * mismatched existing header must fail rather than silently pass through to
     * plain SQLite.
     */
    if (header && std::strncmp(header, config->header, 13u) != 0) {
        return SQLITE_NOTADB;
    }

    methods->zHdr = config->header;
    methods->pCtx = config;
    methods->xCompressClose = noop_close;

    switch (config->codec) {
        case ESDB_CODEC_DEFLATE:
            methods->xCompressBound = zlib_bound;
            methods->xCompress = zlib_compress;
            methods->xUncompress = zlib_uncompress;
            return SQLITE_OK;
#if defined(ESDB_HAS_ZSTD)
        case ESDB_CODEC_ZSTD:
            methods->xCompressBound = zstd_bound;
            methods->xCompress = zstd_compress;
            methods->xUncompress = zstd_uncompress;
            return SQLITE_OK;
#endif
        default:
            return SQLITE_MISUSE;
    }
}

bool normalize_level(
    esdb_codec codec,
    int requested,
    int *out_level) noexcept {
    if (!out_level) return false;

    if (codec == ESDB_CODEC_DEFLATE) {
        if (requested == 0) {
            *out_level = Z_DEFAULT_COMPRESSION;
            return true;
        }
        if (requested < Z_NO_COMPRESSION || requested > Z_BEST_COMPRESSION) {
            return false;
        }
        *out_level = requested;
        return true;
    }

#if defined(ESDB_HAS_ZSTD)
    if (codec == ESDB_CODEC_ZSTD) {
        if (requested == 0) {
            *out_level = ZSTD_CLEVEL_DEFAULT;
            return true;
        }
        if (requested < ZSTD_minCLevel() || requested > ZSTD_maxCLevel()) {
            return false;
        }
        *out_level = requested;
        return true;
    }
#endif

    return false;
}

const char *ensure_zipvfs(
    esdb_codec codec,
    int requested_level,
    esdb_error *error) noexcept {
    int level = 0;
    if (!normalize_level(codec, requested_level, &level)) {
        set_error(
            error,
            codec == ESDB_CODEC_ZSTD && !zipvfs_zstd_compiled()
                ? ESDB_ERR_UNSUPPORTED
                : ESDB_ERR_INVALID_ARGUMENT,
            ESDB_PHASE_OPEN,
            nullptr,
            SQLITE_MISUSE,
            codec == ESDB_CODEC_ZSTD && !zipvfs_zstd_compiled()
                ? "ZIPVFS Zstd support is not compiled into this ESDB build"
                : "compression level is outside the selected codec range");
        return nullptr;
    }

    try {
        const std::pair<std::uint32_t, int> key(
            static_cast<std::uint32_t>(codec), level);
        std::lock_guard<NoThrowMutex> lock(zipvfs_registry_mutex());
        auto &entries = zipvfs_registry();
        const auto found = entries.find(key);
        if (found != entries.end()) {
            return found->second->vfs_name.c_str();
        }

        std::unique_ptr<CodecConfig> config(new (std::nothrow) CodecConfig());
        if (!config) {
            set_error(
                error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_OPEN,
                nullptr, SQLITE_NOMEM, "failed to allocate ZIPVFS codec config");
            return nullptr;
        }

        config->codec = codec;
        config->level = level;
        config->header =
            codec == ESDB_CODEC_DEFLATE ? "ESDBZLIB1" : "ESDBZSTD1";

        char name[64];
        std::snprintf(
            name,
            sizeof(name),
            "esdb-zv-%s-%d",
            codec == ESDB_CODEC_DEFLATE ? "deflate" : "zstd",
            level);
        config->vfs_name = name;

        CodecConfig *raw = config.get();
        const auto inserted = entries.emplace(key, std::move(config));
        if (!inserted.second) {
            return inserted.first->second->vfs_name.c_str();
        }

        const int rc = zipvfs_create_vfs_v3(
            raw->vfs_name.c_str(),
            nullptr,
            raw,
            zipvfs_autodetect);
        if (rc != SQLITE_OK) {
            const char *message = zipvfs_errmsg(rc);
            entries.erase(inserted.first);
            set_error(
                error,
                map_sqlite_status(rc),
                ESDB_PHASE_OPEN,
                nullptr,
                rc,
                message && message[0]
                    ? message
                    : "failed to register ZIPVFS provider");
            return nullptr;
        }

        return raw->vfs_name.c_str();
    } catch (const std::bad_alloc &) {
        set_error(
            error, ESDB_ERR_OUT_OF_MEMORY, ESDB_PHASE_OPEN,
            nullptr, SQLITE_NOMEM, "failed to allocate ZIPVFS registry state");
        return nullptr;
    } catch (...) {
        set_error(
            error, ESDB_ERR_INTERNAL, ESDB_PHASE_OPEN,
            nullptr, SQLITE_ERROR, "unexpected exception registering ZIPVFS");
        return nullptr;
    }
}

}  // namespace

#endif  // ESDB_HAS_ZIPVFS

bool zipvfs_compiled() noexcept {
#if defined(ESDB_HAS_ZIPVFS)
    return true;
#else
    return false;
#endif
}

bool zipvfs_zstd_compiled() noexcept {
#if defined(ESDB_HAS_ZIPVFS) && defined(ESDB_HAS_ZSTD)
    return true;
#else
    return false;
#endif
}

esdb_status storage_vfs_select(
    const esdb_open_options &options,
    const char **out_vfs_name,
    esdb_error *error) noexcept {
    clear_error(error);
    if (!out_vfs_name) {
        set_error(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN,
            nullptr, SQLITE_MISUSE, "out_vfs_name is required");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    *out_vfs_name = nullptr;

    if (options.storage_mode != ESDB_STORAGE_COMPRESSED) {
        return ESDB_OK;
    }

#if !defined(ESDB_HAS_ZIPVFS)
    set_error(
        error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_OPEN,
        nullptr, SQLITE_NOTFOUND,
        "no ZIPVFS provider is compiled into this ESDB build");
    return ESDB_ERR_UNSUPPORTED;
#else
    const char *name = ensure_zipvfs(
        options.compression_codec,
        options.compression_level,
        error);
    if (!name) {
        return error && error->status != ESDB_OK
            ? error->status
            : ESDB_ERR_UNSUPPORTED;
    }
    *out_vfs_name = name;
    return ESDB_OK;
#endif
}

esdb_status storage_verify_open(
    sqlite3 *database,
    const esdb_open_options &options,
    esdb_error *error) noexcept {
    clear_error(error);
    if (!database) {
        set_error(
            error, ESDB_ERR_INVALID_ARGUMENT, ESDB_PHASE_OPEN,
            nullptr, SQLITE_MISUSE, "database is required");
        return ESDB_ERR_INVALID_ARGUMENT;
    }
    if (options.storage_mode != ESDB_STORAGE_COMPRESSED) {
        return ESDB_OK;
    }

#if !defined(ESDB_HAS_ZIPVFS)
    set_error(
        error, ESDB_ERR_UNSUPPORTED, ESDB_PHASE_OPEN,
        database, SQLITE_NOTFOUND,
        "compressed database opened without ZIPVFS support");
    return ESDB_ERR_UNSUPPORTED;
#else
    ZipvfsStat stat{};
    const int rc = sqlite3_file_control(
        database, "main", ZIPVFS_CTRL_STAT, &stat);
    if (rc != SQLITE_OK) {
        set_error(
            error,
            rc == SQLITE_NOTFOUND ? ESDB_ERR_UNSUPPORTED : map_sqlite_status(rc),
            ESDB_PHASE_OPEN,
            database,
            rc,
            rc == SQLITE_NOTFOUND
                ? "compressed storage request did not resolve to a ZIPVFS database"
                : "ZIPVFS verification failed");
        return error ? error->status : map_sqlite_status(rc);
    }
    return ESDB_OK;
#endif
}

}  // namespace esdb_detail
