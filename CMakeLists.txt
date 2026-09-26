cmake_minimum_required(VERSION 3.20)

project(ESDB VERSION 0.2.1 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)

set(_ESDB_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(_ESDB_TOP_LEVEL ON)
endif()
set(_ESDB_EXTERNALOBJECT_DEFAULT OFF)
if(_ESDB_TOP_LEVEL AND WIN32)
    set(_ESDB_EXTERNALOBJECT_DEFAULT ON)
endif()

option(ESDB_BUILD_TESTS "Build ESDB tests" ${_ESDB_TOP_LEVEL})
option(ESDB_BUILD_BENCHMARKS "Build ESDB ORM performance benchmarks" OFF)
option(ESDB_BUILD_EXTERNALOBJECT "Build the ESABI ExternalObject adapter" ${_ESDB_EXTERNALOBJECT_DEFAULT})
option(ESDB_BUILD_STATIC "Also build an isolated static ESDB core" OFF)
option(ESDB_INSTALL "Generate ESDB install/package rules" ${_ESDB_TOP_LEVEL})
option(ESDB_SQLITE_ENABLE_API_ARMOR "Enable SQLITE_ENABLE_API_ARMOR" ON)
set(ESDB_ESABI_SOURCE_DIR "" CACHE PATH "Optional path to an ESABI source tree")

# ZIPVFS is proprietary SQLite source and is never vendored into public ESDB.
# A licensee may point ESDB at their private zipvfs.c + zipvfs.h checkout. ESDB
# then combines the pinned public SQLite amalgamation with that source exactly
# as SQLite's ZIPVFS build documentation requires.
set(ESDB_ZIPVFS_SOURCE "" CACHE FILEPATH "Licensed ZIPVFS zipvfs.c (optional)")
set(ESDB_ZIPVFS_INCLUDE_DIR "" CACHE PATH "Directory containing licensed zipvfs.h")
set(ESDB_ZSTD_INCLUDE_DIR "" CACHE PATH "Optional directory containing zstd.h for ZIPVFS Zstd")
set(ESDB_ZSTD_LIBRARY "" CACHE FILEPATH "Optional Zstd library for ZIPVFS Zstd")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SQLitePin.cmake")
set(ESDB_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
esdb_verify_sqlite_tree("${ESDB_SQLITE_DIR}")

set(_ESDB_SQLITE_INCLUDE_DIR "${ESDB_SQLITE_DIR}")
set(_ESDB_SQLITE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
set(_ESDB_ZIPVFS_ENABLED OFF)
set(_ESDB_ZSTD_ENABLED OFF)

if(ESDB_ZIPVFS_SOURCE)
    if(NOT EXISTS "${ESDB_ZIPVFS_SOURCE}")
        message(FATAL_ERROR "ESDB_ZIPVFS_SOURCE does not exist: ${ESDB_ZIPVFS_SOURCE}")
    endif()
    if(NOT ESDB_ZIPVFS_INCLUDE_DIR OR
       NOT EXISTS "${ESDB_ZIPVFS_INCLUDE_DIR}/zipvfs.h")
        message(FATAL_ERROR
            "ESDB_ZIPVFS_INCLUDE_DIR must contain the licensed zipvfs.h")
    endif()

    find_package(ZLIB REQUIRED)
    set(_ESDB_ZIPVFS_ENABLED ON)
    set(_ESDB_SQLITE_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/esdb-sqlite-zipvfs.c")
    add_custom_command(
        OUTPUT "${_ESDB_SQLITE_SOURCE}"
        COMMAND "${CMAKE_COMMAND}"
            -DESDB_SQLITE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c"
            -DESDB_ZIPVFS_SOURCE="${ESDB_ZIPVFS_SOURCE}"
            -DESDB_OUTPUT="${_ESDB_SQLITE_SOURCE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CombineZipvfs.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c"
            "${ESDB_ZIPVFS_SOURCE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CombineZipvfs.cmake"
        VERBATIM
    )

    if(ESDB_ZSTD_INCLUDE_DIR OR ESDB_ZSTD_LIBRARY)
        if(NOT ESDB_ZSTD_INCLUDE_DIR OR
           NOT EXISTS "${ESDB_ZSTD_INCLUDE_DIR}/zstd.h" OR
           NOT ESDB_ZSTD_LIBRARY OR
           NOT EXISTS "${ESDB_ZSTD_LIBRARY}")
            message(FATAL_ERROR
                "ZIPVFS Zstd requires both ESDB_ZSTD_INCLUDE_DIR (with zstd.h) "
                "and ESDB_ZSTD_LIBRARY")
        endif()
        set(_ESDB_ZSTD_ENABLED ON)
    endif()
endif()

set_source_files_properties(${_ESDB_SQLITE_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_THREADSAFE=1;SQLITE_DEFAULT_FOREIGN_KEYS=1;SQLITE_DQS=0;SQLITE_ENABLE_DBSTAT_VTAB=1;SQLITE_USE_URI=1"
)
if(_ESDB_ZIPVFS_ENABLED)
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY
        COMPILE_DEFINITIONS SQLITE_ENABLE_ZIPVFS=1)
endif()
if(ESDB_SQLITE_ENABLE_API_ARMOR)
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY
        COMPILE_DEFINITIONS SQLITE_ENABLE_API_ARMOR=1)
endif()
# Keep SQLite's normal public API surface intact because ESDB installs the
# matching sqlite3.h for controlled native-handle consumers. ESDB itself never
# enables extension loading; native callers must opt into SQLite's mechanism.
if(MSVC)
    # esdb_native_handle() is a documented native SQLite escape hatch and the
    # installed package ships the matching sqlite3.h. Export SQLite's public
    # API from ESDBCore on Windows so build/install consumers can actually call
    # that documented API against the same SQLite image that owns the handle.
    # POSIX shared libraries export these symbols by default.
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY
        COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS "SQLITE_API=__declspec(dllexport)")
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY COMPILE_OPTIONS /W0 /Brepro)
else()
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY COMPILE_OPTIONS -w)
endif()

set(_ESDB_CORE_SOURCES
    src/esdb_runtime.cpp
    src/esdb_store.cpp
    src/esdb_object_store.cpp
    src/esdb_value.cpp
    src/esdb_zipvfs.cpp
    ${_ESDB_SQLITE_SOURCE}
)

# The shared core is the canonical Adobe deployment. Native plug-ins and the
# ExternalObject adapter must resolve this same module when they need to share
# process-memory Store state.
add_library(esdb SHARED ${_ESDB_CORE_SOURCES})
add_library(ESDB::esdb ALIAS esdb)

target_compile_features(esdb PUBLIC cxx_std_11)
target_link_libraries(esdb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(esdb
    PRIVATE ESDB_BUILD_DLL=1
    INTERFACE ESDB_USE_DLL=1
)
set_target_properties(esdb PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME ESDBCore
    ARCHIVE_OUTPUT_NAME esdb
)

target_include_directories(esdb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${_ESDB_SQLITE_INCLUDE_DIR}
)
if(_ESDB_ZIPVFS_ENABLED)
    target_include_directories(esdb PRIVATE ${ESDB_ZIPVFS_INCLUDE_DIR})
    target_compile_definitions(esdb PRIVATE ESDB_HAS_ZIPVFS=1)
    target_link_libraries(esdb PRIVATE ZLIB::ZLIB)
    if(_ESDB_ZSTD_ENABLED)
        target_include_directories(esdb PRIVATE ${ESDB_ZSTD_INCLUDE_DIR})
        target_compile_definitions(esdb PRIVATE ESDB_HAS_ZSTD=1)
        target_link_libraries(esdb PRIVATE ${ESDB_ZSTD_LIBRARY})
    endif()
endif()

if(MSVC)
    target_compile_definitions(esdb PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(esdb PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/Brepro>
    )
    target_link_options(esdb PRIVATE /Brepro)
else()
    target_compile_options(esdb PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>)
endif()

# A static core remains available for standalone/native consumers that prefer a
# self-contained image. Its Store registry is intentionally isolated per image;
# it is not the deployment for cross-surface Adobe state sharing.
if(ESDB_BUILD_STATIC)
    add_library(esdb_static STATIC ${_ESDB_CORE_SOURCES})
    add_library(ESDB::static ALIAS esdb_static)
    target_compile_features(esdb_static PUBLIC cxx_std_11)
    target_link_libraries(esdb_static PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    target_include_directories(esdb_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${_ESDB_SQLITE_INCLUDE_DIR}
    )
    set_target_properties(esdb_static PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME esdb_static
    )
    if(_ESDB_ZIPVFS_ENABLED)
        target_include_directories(esdb_static PRIVATE ${ESDB_ZIPVFS_INCLUDE_DIR})
        target_compile_definitions(esdb_static PRIVATE ESDB_HAS_ZIPVFS=1)
        target_link_libraries(esdb_static PRIVATE ZLIB::ZLIB)
        if(_ESDB_ZSTD_ENABLED)
            target_include_directories(esdb_static PRIVATE ${ESDB_ZSTD_INCLUDE_DIR})
            target_compile_definitions(esdb_static PRIVATE ESDB_HAS_ZSTD=1)
            target_link_libraries(esdb_static PRIVATE ${ESDB_ZSTD_LIBRARY})
        endif()
    endif()
    if(MSVC)
        target_compile_definitions(esdb_static PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(esdb_static PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/W4>
            $<$<COMPILE_LANGUAGE:CXX>:/Brepro>
        )
        set_property(TARGET esdb_static APPEND PROPERTY STATIC_LIBRARY_OPTIONS /Brepro)
    else()
        target_compile_options(esdb_static PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>)
    endif()
endif()

if(ESDB_BUILD_TESTS)
    enable_testing()
endif()

if(ESDB_BUILD_EXTERNALOBJECT)
    find_package(esabi 0.3.1 EXACT CONFIG QUIET)
    if(NOT TARGET esabi::esabi)
        if(ESDB_ESABI_SOURCE_DIR)
            add_subdirectory("${ESDB_ESABI_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/esabi" EXCLUDE_FROM_ALL)
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../esabi/CMakeLists.txt")
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../esabi" "${CMAKE_CURRENT_BINARY_DIR}/_deps/esabi" EXCLUDE_FROM_ALL)
        else()
            include(FetchContent)
            # ESABI 0.3.1, pinned by immutable commit rather than a mutable tag.
            FetchContent_Declare(esabi
                GIT_REPOSITORY https://github.com/thelabcorner/esabi.git
                GIT_TAG 400fefa14c1c09c0e51555f8e834975bdddbdb1d
            )
            FetchContent_MakeAvailable(esabi)
        endif()
    endif()

    add_library(esdb_externalobject SHARED
        adapters/externalobject/esdb_externalobject.cpp
        adapters/externalobject/esdb_delay_load.cpp
    )
    target_link_libraries(esdb_externalobject PRIVATE ESDB::esdb esabi::esabi)
    target_include_directories(esdb_externalobject PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${_ESDB_SQLITE_INCLUDE_DIR}
    )
    set_target_properties(esdb_externalobject PROPERTIES
        OUTPUT_NAME "ESDB"
        ARCHIVE_OUTPUT_NAME "esdb_externalobject"
    )
    if(MSVC)
        target_compile_options(esdb_externalobject PRIVATE /W4 /Brepro)
        # ESDBCore is side-by-side with the ExternalObject adapter. Delay-load it
        # so the adapter can resolve the core relative to its own module path
        # instead of depending on Illustrator's process-wide DLL search path.
        target_link_libraries(esdb_externalobject PRIVATE delayimp)
        target_link_options(esdb_externalobject PRIVATE
            /Brepro
            /DELAYLOAD:ESDBCore.dll
        )
        # Suppress PE/linker timestamps and other avoidable build-to-build drift.
    else()
        target_compile_options(esdb_externalobject PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(ESDB_BUILD_TESTS)
        add_executable(esdb_externalobject_smoke tests/externalobject_smoke.cpp)
        target_link_libraries(esdb_externalobject_smoke PRIVATE esdb_externalobject ESDB::esdb esabi::esabi)
        target_include_directories(esdb_externalobject_smoke PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/adapters/externalobject
        )
        add_test(NAME esdb_externalobject_smoke COMMAND esdb_externalobject_smoke)
    endif()
endif()

if(ESDB_BUILD_TESTS)
    add_executable(esdb_runtime_smoke tests/runtime_smoke.cpp)
    target_link_libraries(esdb_runtime_smoke PRIVATE ESDB::esdb)
    add_test(NAME esdb_runtime_smoke COMMAND esdb_runtime_smoke)

    add_executable(esdb_cpp_smoke tests/cpp_smoke.cpp)
    target_link_libraries(esdb_cpp_smoke PRIVATE ESDB::esdb)
    add_test(NAME esdb_cpp_smoke COMMAND esdb_cpp_smoke)

    add_executable(esdb_hardening tests/hardening.cpp)
    target_link_libraries(esdb_hardening PRIVATE ESDB::esdb)
    add_test(NAME esdb_hardening COMMAND esdb_hardening)

    add_executable(esdb_store_smoke tests/store_smoke.cpp)
    target_link_libraries(esdb_store_smoke PRIVATE ESDB::esdb)
    add_test(NAME esdb_store_smoke COMMAND esdb_store_smoke)

    add_executable(esdb_crash_smoke tests/crash_smoke.cpp)
    target_link_libraries(esdb_crash_smoke PRIVATE ESDB::esdb)
    add_test(NAME esdb_crash_smoke COMMAND esdb_crash_smoke)

    add_executable(esdb_multiprocess_smoke tests/multiprocess_smoke.cpp)
    target_link_libraries(esdb_multiprocess_smoke PRIVATE ESDB::esdb)
    add_test(NAME esdb_multiprocess_smoke COMMAND esdb_multiprocess_smoke)

    add_executable(esdb_c_header_smoke tests/c_header_smoke.c)
    target_link_libraries(esdb_c_header_smoke PRIVATE ESDB::esdb)
    set_property(TARGET esdb_c_header_smoke PROPERTY LINKER_LANGUAGE CXX)
    add_test(NAME esdb_c_header_smoke COMMAND esdb_c_header_smoke)
endif()

if(ESDB_BUILD_BENCHMARKS)
    add_executable(esdb_orm_bench_generated bench/orm_bench_generated.cpp)
    target_link_libraries(esdb_orm_bench_generated PRIVATE ESDB::esdb)
    target_include_directories(esdb_orm_bench_generated PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${_ESDB_SQLITE_INCLUDE_DIR}
    )
    target_compile_features(esdb_orm_bench_generated PRIVATE cxx_std_11)

    add_executable(esdb_orm_bench_handwritten bench/orm_bench_handwritten.cpp)
    target_link_libraries(esdb_orm_bench_handwritten PRIVATE ESDB::esdb)
    target_include_directories(esdb_orm_bench_handwritten PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${_ESDB_SQLITE_INCLUDE_DIR}
    )
    target_compile_features(esdb_orm_bench_handwritten PRIVATE cxx_std_11)

    if(MSVC)
        target_compile_options(esdb_orm_bench_generated PRIVATE /W4 /O2 /Brepro)
        target_compile_options(esdb_orm_bench_handwritten PRIVATE /W4 /O2 /Brepro)
    else()
        target_compile_options(esdb_orm_bench_generated PRIVATE -Wall -Wextra -Wpedantic -O3)
        target_compile_options(esdb_orm_bench_handwritten PRIVATE -Wall -Wextra -Wpedantic -O3)
    endif()
endif()

if(ESDB_INSTALL)
install(TARGETS esdb
    EXPORT esdbTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(ESDB_BUILD_EXTERNALOBJECT)
    install(TARGETS esdb_externalobject
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES extendscript/esdb.jsx
        DESTINATION ${CMAKE_INSTALL_DATADIR}/esdb/extendscript
    )
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Consumers using esdb_native_handle() for controlled SQLite access can include
# the exact declaration set that matches ESDB's pinned amalgamation.
install(FILES third_party/sqlite/sqlite3.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/esdb
    RENAME sqlite3.h
)

configure_package_config_file(
    cmake/esdbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/esdbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/esdb
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/esdbConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/esdbConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/esdbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/esdb
)
install(EXPORT esdbTargets
    FILE esdbTargets.cmake
    NAMESPACE ESDB::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/esdb
)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/esdb)
install(FILES README.md CHANGELOG.md THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/esdb
)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/esdb
    FILES_MATCHING PATTERN "*.md"
)

set(CPACK_PACKAGE_NAME "ESDB")
set(CPACK_PACKAGE_VENDOR "TheDabCorner LLC")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
    "ESDB-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PROJECT_CONFIG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackProjectConfig.cmake")
set(CPACK_POST_BUILD_SCRIPTS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackNormalize.cmake")
include(CPack)
endif()
