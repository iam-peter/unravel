# A small, deliberately flawed program for trying unravel: layers that
# depend on each other, a duplicated helper, a polymorphic base without a
# virtual destructor, global state, recursion and dead code.
cmake_minimum_required(VERSION 3.16)
project(Warehouse LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_executable(warehouse
    app/main.cpp app/cli.cpp
    core/item.cpp core/inventory.cpp core/pricing.cpp
    storage/store.cpp storage/csvstore.cpp storage/memorystore.cpp
    net/server.cpp)
target_include_directories(warehouse PRIVATE ${CMAKE_SOURCE_DIR})
