include_directories(${ENGINE_COMMON_INCLUDES} ${ENGINE_BLOCKCACHE_INCLUDE} ${ENGINE_PRIMPROC_INCLUDE})

if(COLUMNSTORE_ASAN_FOR_UNITTESTS)
    my_check_and_set_compiler_flag("-U_FORTIFY_SOURCE" DEBUG RELWITHDEBINFO)
    my_check_and_set_compiler_flag("-fsanitize=address -fsanitize-address-use-after-scope -fPIC")
endif()

if(WITH_UNITTESTS)
    cmake_policy(SET CMP0054 NEW)
    set(EXTERNAL_INSTALL_LOCATION ${CMAKE_BINARY_DIR}/external)
    ExternalProject_Add(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest
        GIT_TAG release-1.12.0
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${EXTERNAL_INSTALL_LOCATION} -DBUILD_SHARED_LIBS=ON
    )

    # this is a workaround for gtest's bug still not closed https://github.com/google/googletest/issues/3659
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        my_check_and_set_compiler_flag("-U__cpp_char8_t")
    endif()

    include_directories(${EXTERNAL_INSTALL_LOCATION}/include)
    # lib64 for RPM-based distros
    link_directories(${EXTERNAL_INSTALL_LOCATION}/lib ${EXTERNAL_INSTALL_LOCATION}/lib64)
    set(GTEST_LIBRARIES gtest gtest_main pthread)
    include(GoogleTest)

    # GoogleTest tests
    add_executable(rowgroup_tests rowgroup-tests.cpp)
    target_compile_options(rowgroup_tests PRIVATE -Wno-sign-compare)
    add_dependencies(rowgroup_tests googletest)
    columnstore_link(rowgroup_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_EXEC_LIBS})
    gtest_add_tests(TARGET rowgroup_tests TEST_PREFIX columnstore:)

    add_executable(rewritetest rewritetest.cpp)
    add_dependencies(rewritetest googletest)
    columnstore_link(rewritetest ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_EXEC_LIBS})
    gtest_add_tests(TARGET rewritetest TEST_PREFIX columnstore:)

    add_executable(mcs_decimal_tests mcs_decimal-tests.cpp)
    add_dependencies(mcs_decimal_tests googletest)
    columnstore_link(mcs_decimal_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_EXEC_LIBS})
    gtest_add_tests(TARGET mcs_decimal_tests TEST_PREFIX columnstore:)

    add_executable(dataconvert_tests dataconvert-tests.cpp)
    add_dependencies(dataconvert_tests googletest)
    columnstore_link(dataconvert_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_EXEC_LIBS})
    gtest_add_tests(TARGET dataconvert_tests TEST_PREFIX columnstore:)

    add_executable(rebuild_em_tests rebuild-em-tests.cpp)
    add_dependencies(rebuild_em_tests googletest)
    columnstore_link(rebuild_em_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_WRITE_LIBS})
    gtest_add_tests(TARGET rebuild_em_tests TEST_PREFIX columnstore:)

    add_executable(compression_tests compression-tests.cpp)
    add_dependencies(compression_tests googletest)
    columnstore_link(compression_tests ${ENGINE_LDFLAGS} ${GTEST_LIBRARIES} ${ENGINE_WRITE_LIBS})
    gtest_add_tests(TARGET compression_tests TEST_PREFIX columnstore:)

    add_executable(column_scan_filter_tests primitives_column_scan_and_filter.cpp)
    target_compile_options(column_scan_filter_tests PRIVATE -Wno-error -Wno-sign-compare)
    add_dependencies(column_scan_filter_tests googletest)
    columnstore_link(column_scan_filter_tests ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES} processor dbbc)
    gtest_add_tests(TARGET column_scan_filter_tests TEST_PREFIX columnstore:)

    add_executable(simd_processors simd_processors.cpp)
    target_compile_options(simd_processors PRIVATE -Wno-error)
    add_dependencies(simd_processors googletest)
    columnstore_link(simd_processors ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES} processor dbbc)
    gtest_add_tests(TARGET simd_processors TEST_PREFIX columnstore:)

    add_executable(fair_threadpool_test fair_threadpool.cpp)
    add_dependencies(fair_threadpool_test googletest)
    columnstore_link(fair_threadpool_test ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES} processor dbbc)
    gtest_add_tests(TARGET fair_threadpool_test TEST_PREFIX columnstore:)

    add_executable(counting_allocator counting_allocator.cpp)
    target_compile_options(counting_allocator PRIVATE -Wno-sign-compare)
    add_dependencies(counting_allocator googletest)
    columnstore_link(counting_allocator ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES})
    gtest_add_tests(TARGET counting_allocator TEST_PREFIX columnstore:)

    add_executable(poolallocator poolallocator.cpp)
    target_compile_options(poolallocator PRIVATE -Wno-sign-compare)
    add_dependencies(poolallocator googletest)
    columnstore_link(poolallocator ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES})
    gtest_add_tests(TARGET poolallocator TEST_PREFIX columnstore:)

    add_executable(stlpoolallocator stlpoolallocator.cpp)
    target_compile_options(stlpoolallocator PRIVATE -Wno-sign-compare)
    add_dependencies(stlpoolallocator googletest)
    columnstore_link(stlpoolallocator ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${GTEST_LIBRARIES})
    gtest_add_tests(TARGET stlpoolallocator TEST_PREFIX columnstore:)

    add_executable(comparators_tests comparators-tests.cpp)
    columnstore_link(comparators_tests ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${CPPUNIT_LIBRARIES} cppunit)
    add_test(NAME columnstore:comparators_tests COMMAND comparators_tests)

    add_executable(bytestream bytestream.cpp)
    columnstore_link(bytestream ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${CPPUNIT_LIBRARIES} cppunit)
    add_test(NAME columnstore:bytestream COMMAND bytestream)

    add_executable(idbdatafile_test BufferedFile.cpp)
    columnstore_link(idbdatafile_test ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} gtest gtest_main)
    add_test(NAME columnstore:idbdatafile_test COMMAND idbdatafile_test)

    # standalone EM routines test
    add_executable(brm_em_standalone brm-em-standalone.cpp)
    columnstore_link(brm_em_standalone ${ENGINE_LDFLAGS} ${ENGINE_WRITE_LIBS} ${CPPUNIT_LIBRARIES} cppunit)
    add_test(NAME columnstore:brm_em_standalone COMMAND brm_em_standalone)
    set_tests_properties(columnstore:brm_em_standalone PROPERTIES DISABLED True)
endif()

if(WITH_MICROBENCHMARKS AND (NOT CMAKE_BUILD_TYPE STREQUAL "Debug"))
    find_package(benchmark REQUIRED)
    add_executable(primitives_scan_bench primitives_scan_bench.cpp)
    target_include_directories(
        primitives_scan_bench PUBLIC ${ENGINE_COMMON_INCLUDES} ${ENGINE_BLOCKCACHE_INCLUDE} ${ENGINE_PRIMPROC_INCLUDE}
    )
    columnstore_link(
        primitives_scan_bench
        ${ENGINE_LDFLAGS}
        ${ENGINE_WRITE_LIBS}
        ${GTEST_LIBRARIES}
        processor
        dbbc
        benchmark::benchmark
    )
    add_test(NAME columnstore_microbenchmarks:primitives_scan_bench, COMMAND primitives_scan_bench)
endif()
