# One executable per component keeps failures easy to locate. Phase 4 adds
# test_server.cpp here, Phase 5 test_thread_pool.cpp, and so on.
function(vcache_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE vcache_core)

    # So test_framework.h resolves from this directory.
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME ${name} COMMAND ${name})

    # A socket bug should fail the suite, not wedge it forever.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

vcache_add_test(test_hashtable)
vcache_add_test(test_database)
vcache_add_test(test_command_parser)
vcache_add_test(test_command_executor)
vcache_add_test(test_thread_pool)
vcache_add_test(test_expiration)
vcache_add_test(test_persistence)
vcache_add_test(test_eviction)
vcache_add_test(test_concurrency)
vcache_add_test(test_server)
vcache_add_test(test_client)
