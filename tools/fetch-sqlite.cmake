cmake_minimum_required(VERSION 3.20)

# Cross-platform repair/update tool for the vendored SQLite amalgamation.
# Usage:
#   cmake -P tools/fetch-sqlite.cmake
#   cmake -DESDB_SQLITE_FORCE=ON -P tools/fetch-sqlite.cmake

get_filename_component(ESDB_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${ESDB_ROOT}/cmake/SQLitePin.cmake")

set(_dest "${ESDB_ROOT}/third_party/sqlite")
set(_required
    "${_dest}/sqlite3.c"
    "${_dest}/sqlite3.h"
    "${_dest}/sqlite3ext.h"
)

set(_complete TRUE)
foreach(_path IN LISTS _required)
    if(NOT EXISTS "${_path}")
        set(_complete FALSE)
    endif()
endforeach()

if(_complete AND NOT ESDB_SQLITE_FORCE)
    esdb_verify_sqlite_tree("${_dest}")
    message(STATUS "SQLite ${ESDB_SQLITE_VERSION} vendored tree is present and verified")
    return()
endif()

set(_work "${CMAKE_CURRENT_BINARY_DIR}/esdb-sqlite-fetch-${ESDB_SQLITE_PRODUCT}")
set(_archive "${_work}/sqlite-amalgamation-${ESDB_SQLITE_PRODUCT}.zip")
set(_extract "${_work}/extract")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}" "${_extract}")

message(STATUS "Downloading ${ESDB_SQLITE_URL}")
file(DOWNLOAD
    "${ESDB_SQLITE_URL}"
    "${_archive}"
    EXPECTED_HASH "SHA3_256=${ESDB_SQLITE_ARCHIVE_SHA3_256}"
    TLS_VERIFY ON
    STATUS _download_status
)
list(GET _download_status 0 _download_code)
if(NOT _download_code EQUAL 0)
    list(GET _download_status 1 _download_message)
    file(REMOVE_RECURSE "${_work}")
    message(FATAL_ERROR "SQLite download failed: ${_download_message}")
endif()

file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extract}")
file(GLOB _roots LIST_DIRECTORIES TRUE "${_extract}/sqlite-amalgamation-*")
list(LENGTH _roots _root_count)
if(NOT _root_count EQUAL 1)
    file(REMOVE_RECURSE "${_work}")
    message(FATAL_ERROR "Unexpected SQLite archive layout: expected exactly one sqlite-amalgamation-* directory")
endif()
list(GET _roots 0 _source)

file(MAKE_DIRECTORY "${_dest}")
foreach(_name sqlite3.c sqlite3.h sqlite3ext.h)
    if(NOT EXISTS "${_source}/${_name}")
        file(REMOVE_RECURSE "${_work}")
        message(FATAL_ERROR "SQLite archive is missing ${_name}")
    endif()
    configure_file("${_source}/${_name}" "${_dest}/${_name}" COPYONLY)
endforeach()

esdb_verify_sqlite_tree("${_dest}")

file(WRITE "${_dest}/.fetched"
    "sqlite_version=${ESDB_SQLITE_VERSION}\n"
    "sqlite_product=${ESDB_SQLITE_PRODUCT}\n"
    "sha3_256=${ESDB_SQLITE_ARCHIVE_SHA3_256}\n"
    "source=${ESDB_SQLITE_URL}\n"
)
file(REMOVE_RECURSE "${_work}")
message(STATUS "Verified and installed SQLite ${ESDB_SQLITE_VERSION} into ${_dest}")
