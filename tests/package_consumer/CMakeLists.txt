cmake_minimum_required(VERSION 3.20)
project(esdb_package_consumer LANGUAGES CXX)

find_package(esdb 0.1 CONFIG REQUIRED)

add_executable(esdb_package_consumer main.cpp)
target_compile_features(esdb_package_consumer PRIVATE cxx_std_17)
target_link_libraries(esdb_package_consumer PRIVATE ESDB::esdb)
