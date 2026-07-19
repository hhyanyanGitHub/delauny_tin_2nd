cmake_minimum_required(VERSION 3.24)
project(dterrain_sdk_consumer LANGUAGES CXX)

find_package(dterrain 1 CONFIG REQUIRED)
add_executable(dterrain_sdk_consumer main.cpp)
target_compile_features(dterrain_sdk_consumer PRIVATE cxx_std_17)
target_link_libraries(dterrain_sdk_consumer PRIVATE dterrain::dterrain)
