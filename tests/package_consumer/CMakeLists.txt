cmake_minimum_required(VERSION 3.20)
project(esdb_package_consumer LANGUAGES C CXX)

find_package(esdb 0.2 CONFIG REQUIRED)

add_executable(esdb_package_consumer main.cpp)
target_compile_features(esdb_package_consumer PRIVATE cxx_std_11)
target_link_libraries(esdb_package_consumer PRIVATE ESDB::esdb)

add_executable(esdb_package_consumer_c main.c)
target_link_libraries(esdb_package_consumer_c PRIVATE ESDB::esdb)

add_executable(esdb_package_consumer_orm main_orm.cpp)
target_compile_features(esdb_package_consumer_orm PRIVATE cxx_std_11)
target_link_libraries(esdb_package_consumer_orm PRIVATE ESDB::esdb)
target_include_directories(
    esdb_package_consumer_orm
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../examples/orm/user/generated"
)
