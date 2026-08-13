# Benchmarks link the same library the server does, so they measure shipping
# code rather than a special build of it.
function(vcache_add_benchmark name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE vcache_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

vcache_add_benchmark(bench_hashtable)
vcache_add_benchmark(bench_database)
vcache_add_benchmark(bench_server)
