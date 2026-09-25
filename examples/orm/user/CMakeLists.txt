cmake_minimum_required(VERSION 3.20)

project(ESDBOrmUserExample LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ESDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ESDB_BUILD_EXTERNALOBJECT OFF CACHE BOOL "" FORCE)
set(ESDB_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(ESDB_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../.."
    "${CMAKE_CURRENT_BINARY_DIR}/esdb-runtime"
    EXCLUDE_FROM_ALL
)

add_executable(esdb_orm_user_smoke main.cpp)
target_link_libraries(esdb_orm_user_smoke PRIVATE ESDB::esdb)
target_include_directories(
    esdb_orm_user_smoke
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/sqlite"
)
target_compile_features(esdb_orm_user_smoke PRIVATE cxx_std_11)

if(MSVC)
    target_compile_options(esdb_orm_user_smoke PRIVATE /W4)
else()
    target_compile_options(esdb_orm_user_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_custom_command(TARGET esdb_orm_user_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:esdb>
            $<TARGET_FILE_DIR:esdb_orm_user_smoke>
    )
endif()

set(_ESDB_USER_ESABI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../esabi")
if(EXISTS "${_ESDB_USER_ESABI_DIR}/CMakeLists.txt")
    add_subdirectory(
        "${_ESDB_USER_ESABI_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/esabi"
        EXCLUDE_FROM_ALL
    )

    add_library(user_orm_bridge SHARED bridge/user_orm_bridge.cpp)
    target_link_libraries(user_orm_bridge PRIVATE ESDB::esdb esabi::esabi)
    target_include_directories(
        user_orm_bridge
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/sqlite"
    )
    target_compile_features(user_orm_bridge PRIVATE cxx_std_11)
    set_target_properties(user_orm_bridge PROPERTIES OUTPUT_NAME "UserOrmBridge")

    if(MSVC)
        target_sources(
            user_orm_bridge
            PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../adapters/externalobject/esdb_delay_load.cpp"
        )
        target_compile_options(user_orm_bridge PRIVATE /W4)
        target_link_libraries(user_orm_bridge PRIVATE delayimp)
        target_link_options(user_orm_bridge PRIVATE /DELAYLOAD:ESDBCore.dll)
    else()
        target_compile_options(user_orm_bridge PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(WIN32)
        add_custom_command(TARGET user_orm_bridge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:esdb>
                $<TARGET_FILE_DIR:user_orm_bridge>
        )
    endif()

    add_executable(esdb_orm_user_bridge_smoke bridge_smoke.cpp)
    target_link_libraries(
        esdb_orm_user_bridge_smoke
        PRIVATE user_orm_bridge ESDB::esdb esabi::esabi
    )
    target_include_directories(
        esdb_orm_user_bridge_smoke
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/sqlite"
    )
    target_compile_features(esdb_orm_user_bridge_smoke PRIVATE cxx_std_11)

    add_executable(esdb_orm_user_bridge_bench bridge_bench.cpp)
    target_link_libraries(
        esdb_orm_user_bridge_bench
        PRIVATE user_orm_bridge ESDB::esdb esabi::esabi
    )
    target_include_directories(
        esdb_orm_user_bridge_bench
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/sqlite"
    )
    target_compile_features(esdb_orm_user_bridge_bench PRIVATE cxx_std_11)

    if(MSVC)
        target_compile_options(esdb_orm_user_bridge_smoke PRIVATE /W4)
        target_compile_options(esdb_orm_user_bridge_bench PRIVATE /W4 /O2)
    else()
        target_compile_options(esdb_orm_user_bridge_smoke PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(esdb_orm_user_bridge_bench PRIVATE -Wall -Wextra -Wpedantic -O3)
    endif()
endif()
