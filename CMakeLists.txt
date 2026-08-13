cmake_minimum_required(VERSION 3.16)

project(vcache
    VERSION 0.1.0
    DESCRIPTION "A Redis-inspired in-memory key-value database"
    LANGUAGES CXX)

# C++17 for std::optional (Entry::expiration) and if constexpr.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++17, never -std=gnu++17

# Generates compile_commands.json so clangd / IDEs get correct include paths.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Benchmarks are meaningless in a debug build, so default to Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(VCACHE_BUILD_TESTS "Build the VCache test suite" ON)
option(VCACHE_BUILD_BENCHMARKS "Build the VCache benchmarks" ON)
option(VCACHE_ENABLE_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(VCACHE_ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(VCACHE_ENABLE_ASAN AND VCACHE_ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be enabled together; use separate build directories")
endif()

# Compiler settings shared by every target, carried automatically to anything
# that links it.
add_library(vcache_options INTERFACE)

if(MSVC)
    target_compile_options(vcache_options INTERFACE /W4 /permissive-)
else()
    target_compile_options(vcache_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(VCACHE_ENABLE_ASAN)
    if(MSVC)
        message(WARNING "VCACHE_ENABLE_ASAN is only wired up for GCC/Clang")
    else()
        target_compile_options(vcache_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(vcache_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

if(VCACHE_ENABLE_TSAN)
    if(MSVC)
        message(WARNING "VCACHE_ENABLE_TSAN is only wired up for GCC/Clang")
    else()
        target_compile_options(vcache_options INTERFACE
            -fsanitize=thread -fno-omit-frame-pointer -g)
        target_link_options(vcache_options INTERFACE -fsanitize=thread)
    endif()
endif()

# Needed by the server tests now, and by the Phase 5 thread pool shortly.
find_package(Threads REQUIRED)

# The database core. Later phases add Persistence.cpp and LRUCache.cpp to this
# list.
add_library(vcache_core
    src/HashTable.cpp
    src/Database.cpp
    src/CommandParser.cpp
    src/CommandExecutor.cpp
    src/ThreadPool.cpp
    src/ExpirationSweeper.cpp
    src/Persistence.cpp
    src/SnapshotScheduler.cpp
    src/Server.cpp
    src/VCacheClient.cpp)

target_include_directories(vcache_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(vcache_core PUBLIC vcache_options Threads::Threads)

# The daemon.
add_executable(vcache-server server/main.cpp)
target_link_libraries(vcache-server PRIVATE vcache_core)

# The interactive client (Discovery Document section 19).
add_executable(vcache-cli client/main.cpp)
target_link_libraries(vcache-cli PRIVATE vcache_core)

if(VCACHE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarks are built but deliberately NOT registered with CTest: they take
# minutes, and a timing result is not a pass/fail. Run them by hand, from a
# Release build, on an otherwise idle machine.
if(VCACHE_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

message(STATUS "VCache ${PROJECT_VERSION} | build: ${CMAKE_BUILD_TYPE} | tests: ${VCACHE_BUILD_TESTS}"
        " | benchmarks: ${VCACHE_BUILD_BENCHMARKS} | asan: ${VCACHE_ENABLE_ASAN} | tsan: ${VCACHE_ENABLE_TSAN}")
