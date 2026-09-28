# Each test file compiles into its own executable so that a crash in one suite
# cannot take the others down with it, and so `ctest` reports failures per unit.

function(fragfs_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE fragfs_core fragfs_warnings)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

fragfs_add_test(smoke_test)
fragfs_add_test(fragment_test)
fragfs_add_test(metadata_test)
fragfs_add_test(serialization_test)
fragfs_add_test(mapper_test)
fragfs_add_test(posix_file_test)
fragfs_add_test(logical_file_test)
fragfs_add_test(create_test)
fragfs_add_test(operations_test)
fragfs_add_test(verify_test)
fragfs_add_test(concurrency_test)
fragfs_add_test(writer_lock_test)
fragfs_add_test(split_test)
fragfs_add_test(assemble_test)
