cmake_minimum_required(VERSION 3.16)
project(Barry CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS         "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g")

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx512f" SUPPORTS_AVX512)
if(SUPPORTS_AVX512)
    add_compile_options(-mavx512f)
    add_compile_definitions(HAVE_AVX512)
endif()

find_package(Threads REQUIRED)
find_package(OpenMP)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(barry STATIC
    src/time_mapper.cpp
    src/arena.cpp
    src/timeline.cpp
    src/vertex_index.cpp
    src/retention.cpp
    src/engine.cpp
    src/algorithms/bfs.cpp
    src/algorithms/pagerank.cpp
    src/algorithms/triangle_counting.cpp
    src/algorithms/betweenness.cpp
    src/algorithms/earliest_arrival.cpp
    src/algorithms/temporal_triangle.cpp
    src/algorithms/sssp.cpp
    src/algorithms/connected_components.cpp
    src/algorithms/kcore.cpp
    src/algorithms/taint.cpp
)
target_link_libraries(barry PUBLIC Threads::Threads)
if(OpenMP_CXX_FOUND)
    target_link_libraries(barry PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(barry PUBLIC BARRY_HAVE_OPENMP)
endif()

# tools
add_executable(barry_preprocess tools/preprocess.cpp)
target_link_libraries(barry_preprocess PRIVATE barry)

add_executable(barry_analyze tools/analyze.cpp)
target_link_libraries(barry_analyze PRIVATE barry)

add_executable(barry_demo tools/demo.cpp)
target_link_libraries(barry_demo PRIVATE barry)

add_executable(barry_record_env tools/record_env.cpp)

add_executable(barry_bin_to_gbbs tools/bin_to_gbbs.cpp)
target_link_libraries(barry_bin_to_gbbs PRIVATE barry)

add_executable(barry_tc_cdm_to_bin tools/tc_cdm_to_bin.cpp)
target_link_libraries(barry_tc_cdm_to_bin PRIVATE barry)

add_executable(barry_provenance_query tools/provenance_query.cpp)
target_link_libraries(barry_provenance_query PRIVATE barry)

add_executable(barry_lanl_to_bin tools/lanl_to_bin.cpp)
target_link_libraries(barry_lanl_to_bin PRIVATE barry)

# benchmarks
foreach(B e1_ingest e2_query e3_compute e4_scale e5_memory e6_cache e7_provenance e8_attack e11_sweep e12_staleness e13_storage_read)
    add_executable(bench_${B} benchmark/bench_${B}.cpp)
    target_link_libraries(bench_${B} PRIVATE barry)
    target_include_directories(bench_${B} PRIVATE ${CMAKE_SOURCE_DIR}/benchmark)
endforeach()

# E9 needs the DuckDB C API; built only when the library is present so the normal
# build stays dependency-free.
set(DUCKDB_ROOT "$ENV{HOME}/opt/duckdb" CACHE PATH "DuckDB C API root")
if(EXISTS "${DUCKDB_ROOT}/duckdb.h" AND EXISTS "${DUCKDB_ROOT}/libduckdb.so")
    foreach(DB e9_columnar e10_retention)
        add_executable(bench_${DB} benchmark/bench_${DB}.cpp)
        target_include_directories(bench_${DB} PRIVATE
            ${CMAKE_SOURCE_DIR}/benchmark ${DUCKDB_ROOT})
        target_link_libraries(bench_${DB} PRIVATE barry "${DUCKDB_ROOT}/libduckdb.so")
        set_target_properties(bench_${DB} PROPERTIES
            BUILD_RPATH "${DUCKDB_ROOT}" INSTALL_RPATH "${DUCKDB_ROOT}")
    endforeach()
    message(STATUS "DuckDB found at ${DUCKDB_ROOT} -- building bench_e9_columnar")
else()
    message(STATUS "DuckDB not found at ${DUCKDB_ROOT} -- skipping bench_e9_columnar")
endif()

# tests
enable_testing()
add_executable(test_smoke tests/test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE barry)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_time_mapper tests/test_time_mapper.cpp)
target_link_libraries(test_time_mapper PRIVATE barry)
add_test(NAME time_mapper COMMAND test_time_mapper)

add_executable(test_arena tests/test_arena.cpp)
target_link_libraries(test_arena PRIVATE barry)
add_test(NAME arena COMMAND test_arena)

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE barry)
add_test(NAME engine COMMAND test_engine)

add_executable(test_taint tests/test_taint.cpp)
target_link_libraries(test_taint PRIVATE barry)
add_test(NAME taint COMMAND test_taint)
