# Canonical SQLite release pin shared by the build and maintenance tooling.
# Facts live in sqlite-pin.json so they are machine-readable outside CMake too.

set(ESDB_SQLITE_PIN_FILE "${CMAKE_CURRENT_LIST_DIR}/sqlite-pin.json")
if(NOT EXISTS "${ESDB_SQLITE_PIN_FILE}")
    message(FATAL_ERROR "ESDB SQLite pin file is missing: ${ESDB_SQLITE_PIN_FILE}")
endif()

file(READ "${ESDB_SQLITE_PIN_FILE}" _esdb_sqlite_pin_json)
string(JSON ESDB_SQLITE_VERSION GET "${_esdb_sqlite_pin_json}" version)
string(JSON ESDB_SQLITE_PRODUCT GET "${_esdb_sqlite_pin_json}" product)
string(JSON ESDB_SQLITE_URL GET "${_esdb_sqlite_pin_json}" url)
string(JSON ESDB_SQLITE_ARCHIVE_SHA3_256 GET "${_esdb_sqlite_pin_json}" archiveSha3_256)
string(JSON ESDB_SQLITE3_C_SHA256 GET "${_esdb_sqlite_pin_json}" files "sqlite3.c")
string(JSON ESDB_SQLITE3_H_SHA256 GET "${_esdb_sqlite_pin_json}" files "sqlite3.h")
string(JSON ESDB_SQLITE3EXT_H_SHA256 GET "${_esdb_sqlite_pin_json}" files "sqlite3ext.h")
unset(_esdb_sqlite_pin_json)

function(esdb_verify_sqlite_tree directory)
    set(_files sqlite3.c sqlite3.h sqlite3ext.h)
    set(_hashes
        "${ESDB_SQLITE3_C_SHA256}"
        "${ESDB_SQLITE3_H_SHA256}"
        "${ESDB_SQLITE3EXT_H_SHA256}"
    )

    list(LENGTH _files _count)
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 0 ${_last})
        list(GET _files ${_index} _name)
        list(GET _hashes ${_index} _expected)
        set(_path "${directory}/${_name}")
        if(NOT EXISTS "${_path}")
            message(FATAL_ERROR
                "Pinned SQLite ${ESDB_SQLITE_VERSION} is incomplete: missing ${_path}. "
                "Restore the vendored dependency or run 'cmake -P tools/fetch-sqlite.cmake'.")
        endif()
        file(SHA256 "${_path}" _actual)
        string(TOLOWER "${_actual}" _actual)
        string(TOLOWER "${_expected}" _expected_lower)
        if(NOT _actual STREQUAL _expected_lower)
            message(FATAL_ERROR
                "Pinned SQLite file hash mismatch for ${_name}. "
                "Expected SHA256 ${_expected_lower}, got ${_actual}.")
        endif()
    endforeach()
endfunction()
