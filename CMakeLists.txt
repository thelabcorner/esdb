cmake_minimum_required(VERSION 3.20)

project(ESDB VERSION 0.1.0 LANGUAGES C CXX)

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
option(ESDB_BUILD_EXTERNALOBJECT "Build the ESABI ExternalObject adapter" ${_ESDB_EXTERNALOBJECT_DEFAULT})
option(ESDB_INSTALL "Generate ESDB install/package rules" ${_ESDB_TOP_LEVEL})
option(ESDB_SQLITE_ENABLE_API_ARMOR "Enable SQLITE_ENABLE_API_ARMOR" ON)
set(ESDB_ESABI_SOURCE_DIR "" CACHE PATH "Optional path to an ESABI source tree")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SQLitePin.cmake")
set(ESDB_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
esdb_verify_sqlite_tree("${ESDB_SQLITE_DIR}")

set(_ESDB_SQLITE_SOURCE third_party/sqlite/sqlite3.c)
set_source_files_properties(${_ESDB_SQLITE_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_THREADSAFE=1;SQLITE_DEFAULT_FOREIGN_KEYS=1;SQLITE_DQS=0;SQLITE_ENABLE_DBSTAT_VTAB=1;SQLITE_USE_URI=1"
)
if(ESDB_SQLITE_ENABLE_API_ARMOR)
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY
        COMPILE_DEFINITIONS SQLITE_ENABLE_API_ARMOR=1)
endif()
# Keep SQLite's normal public API surface intact because ESDB installs the
# matching sqlite3.h for controlled native-handle consumers. ESDB itself never
# enables extension loading; native callers must opt into SQLite's mechanism.
if(MSVC)
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY
        COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS)
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY COMPILE_OPTIONS /W0 /Brepro)
else()
    set_property(SOURCE ${_ESDB_SQLITE_SOURCE} APPEND PROPERTY COMPILE_OPTIONS -w)
endif()

add_library(esdb STATIC
    src/esdb_runtime.cpp
    src/esdb_store.cpp
    src/esdb_value.cpp
    ${_ESDB_SQLITE_SOURCE}
)
add_library(ESDB::esdb ALIAS esdb)

target_compile_features(esdb PUBLIC cxx_std_11)
target_link_libraries(esdb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

set_target_properties(esdb PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME esdb
)

target_include_directories(esdb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ESDB_SQLITE_DIR}
)

if(MSVC)
    target_compile_definitions(esdb PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(esdb PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/Brepro>
    )
    # Suppress archive timestamps; keeping SQLite inside this target also avoids
    # build-directory paths leaking into archive member names.
    set_property(TARGET esdb APPEND PROPERTY STATIC_LIBRARY_OPTIONS /Brepro)
else()
    target_compile_options(esdb PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>)
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
                GIT_TAG 65c9c3ce627a26a89d6bf90547678841df0cf981
            )
            FetchContent_MakeAvailable(esabi)
        endif()
    endif()

    add_library(esdb_externalobject SHARED adapters/externalobject/esdb_externalobject.cpp)
    target_link_libraries(esdb_externalobject PRIVATE ESDB::esdb esabi::esabi)
    target_include_directories(esdb_externalobject PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(esdb_externalobject PROPERTIES
        OUTPUT_NAME "ESDB"
        ARCHIVE_OUTPUT_NAME "esdb_externalobject"
    )
    if(MSVC)
        target_compile_options(esdb_externalobject PRIVATE /W4 /Brepro)
        # Suppress PE/linker timestamps and other avoidable build-to-build drift.
        target_link_options(esdb_externalobject PRIVATE /Brepro)
    else()
        target_compile_options(esdb_externalobject PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(ESDB_BUILD_TESTS)
        add_executable(esdb_externalobject_smoke tests/externalobject_smoke.cpp)
        target_link_libraries(esdb_externalobject_smoke PRIVATE esdb_externalobject esabi::esabi)
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
include(CPack)
endif()
