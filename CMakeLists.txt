cmake_minimum_required(VERSION 3.25)
project(mdbus VERSION 0.2 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Every cost measured here was taken at -O2, not CMake's default -O3.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
add_compile_options(-mcpu=apple-m1)

set(MDBUS_SANITIZE "none" CACHE STRING "none, address (ASan+UBSan) or thread (TSan)")
set_property(CACHE MDBUS_SANITIZE PROPERTY STRINGS none address thread)
if(MDBUS_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
    -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
elseif(MDBUS_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT MDBUS_SANITIZE STREQUAL "none")
  message(FATAL_ERROR "MDBUS_SANITIZE must be none, address or thread")
endif()

# The library, header-only.
# - The warning flags sit on its interface, so every mdbus target gets them and the fetched
#   HdrHistogram code does not.
add_library(mdbus INTERFACE)
target_include_directories(mdbus INTERFACE include)
target_compile_features(mdbus INTERFACE cxx_std_20)
target_compile_options(mdbus INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

# Headers outside the library, included from the repo root as "sim/...", "baseline/...",
# "src/...".
# - sim/ stands in for the exchange; baseline/ holds the designs mdbus is measured against.
# - Programs, tests and benchmarks use them. The mdbus library never does.
add_library(mdbus_repo_headers INTERFACE)
target_include_directories(mdbus_repo_headers INTERFACE ${PROJECT_SOURCE_DIR})
target_link_libraries(mdbus_repo_headers INTERFACE mdbus)

# Programs: the simulated exchange, the feed handler (the bus writer) and a live viewer.
add_executable(mdbus_exchange_sim sim/exchange_sim.cpp)
foreach(program feed_handler watch)
  add_executable(mdbus_${program} src/${program}.cpp)
endforeach()
foreach(program exchange_sim feed_handler watch)
  target_link_libraries(mdbus_${program} PRIVATE mdbus_repo_headers)
endforeach()

# Benchmarks, all written to build/bench.
# - One binary per bus variant (baseline/bus_variants.hpp) and one per feed book (F0..F4,
#   baseline/book_ladder.hpp), plus the dispatch benchmark.
# - The variant is a macro, so each binary measures exactly one design with no runtime switch.
include(FetchContent)
FetchContent_Declare(hdr_histogram
  URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.10.tar.gz
  URL_HASH SHA256=c3b06d077e680d112abf9f027d8a558f1176ee4a55a7c523577833391d8c2249 SYSTEM)
# Only the static library is needed.
foreach(option HDR_HISTOGRAM_BUILD_PROGRAMS HDR_HISTOGRAM_BUILD_SHARED HDR_HISTOGRAM_INSTALL_SHARED
    HDR_HISTOGRAM_INSTALL_STATIC)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(HDR_LOG_REQUIRED DISABLED CACHE STRING "" FORCE)  # no zlib: no log files are written
FetchContent_MakeAvailable(hdr_histogram)
# The zero-warning rule covers mdbus code only.
target_compile_options(hdr_histogram_static PRIVATE -w)

set(bus_variants Mutex Base Pad64 Copy15 HeadPoll)
set(feed_variants F0 F1 F2 F3 F4)
foreach(variant IN LISTS bus_variants)
  add_executable(mdbus_bench_${variant} bench/bus_bench.cpp)
  target_compile_definitions(mdbus_bench_${variant}
    PRIVATE MDBUS_VARIANT=${variant} MDBUS_VARIANT_NAME="${variant}")
endforeach()
foreach(variant IN LISTS feed_variants)
  add_executable(mdbus_bench_${variant} bench/feed_bench.cpp)
  target_compile_definitions(mdbus_bench_${variant} PRIVATE MDBUS_FEED_VARIANT=${variant})
endforeach()
add_executable(mdbus_bench_dispatch bench/dispatch_bench.cpp)
foreach(variant IN LISTS bus_variants feed_variants ITEMS dispatch)
  target_link_libraries(mdbus_bench_${variant} PRIVATE mdbus_repo_headers hdr_histogram_static)
  set_target_properties(mdbus_bench_${variant}
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bench)
endforeach()

# Tests.
# - Release passes -DNDEBUG; the tests undo it so assert and every check stay live.
enable_testing()
add_library(mdbus_test_support INTERFACE)
target_include_directories(mdbus_test_support INTERFACE tests)
target_link_libraries(mdbus_test_support INTERFACE mdbus_repo_headers)
target_compile_options(mdbus_test_support INTERFACE -UNDEBUG)

# add_mdbus_test(<name> [NO_MAIN] [LABELS ...] [DEFINES ...] [ARGS ...] [TIMEOUT s]) builds
# tests/<name>.cpp with the shared tests/test_main.cpp (NO_MAIN: the test has its own main) and
# registers it with ctest (default timeout 60 s).
function(add_mdbus_test name)
  cmake_parse_arguments(ARG "NO_MAIN" "TIMEOUT" "LABELS;DEFINES;ARGS" ${ARGN})
  add_executable(${name} tests/${name}.cpp)
  if(NOT ARG_NO_MAIN)
    target_sources(${name} PRIVATE tests/test_main.cpp)
  endif()
  target_link_libraries(${name} PRIVATE mdbus_test_support)
  target_compile_definitions(${name} PRIVATE ${ARG_DEFINES})
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 60)
  endif()
  add_test(NAME ${name} COMMAND ${name} ${ARG_ARGS})
  set_tests_properties(${name} PROPERTIES TIMEOUT ${ARG_TIMEOUT} LABELS "${ARG_LABELS}")
endfunction()

foreach(test_name messages ring snapshot consumer no_alloc udp_feed)
  add_mdbus_test(${test_name}_test LABELS unit)
endforeach()
# Compile-fail: a BookDelta published without its snapshot must not compile. Each test compiles
# one such call and passes only on Publisher's static_assert message.
set(refused_publish ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only -I${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/tests/publish_refuses_book_delta.cpp)
add_test(NAME publish_refuses_book_delta COMMAND ${refused_publish})
add_test(NAME publish_with_latency_start_refuses_book_delta
  COMMAND ${refused_publish} -DMDBUS_WITH_LATENCY_START)
set_tests_properties(publish_refuses_book_delta publish_with_latency_start_refuses_book_delta
  PROPERTIES LABELS unit
  PASS_REGULAR_EXPRESSION "a BookDelta must go through publish_and_update_snapshot")
foreach(test_name book_vs_naive ladder_and_table book_through_bus)
  add_mdbus_test(${test_name}_test LABELS book TIMEOUT 300)
endforeach()
# Signed overflow traps in ladder_and_table_test in every build, not only under UBSan, so its
# window test fails on an index that overflows even where the wrapped value happens to work.
target_compile_options(ladder_and_table_test PRIVATE -fsanitize=signed-integer-overflow
  -fsanitize-trap=signed-integer-overflow)
# The real simulator, feed handler and viewer processes, with the consumer in the test process.
add_mdbus_test(end_to_end_test LABELS e2e TIMEOUT 120
           DEFINES MDBUS_FEED_HANDLER_PATH="$<TARGET_FILE:mdbus_feed_handler>"
                   MDBUS_EXCHANGE_SIM_PATH="$<TARGET_FILE:mdbus_exchange_sim>"
                   MDBUS_WATCH_PATH="$<TARGET_FILE:mdbus_watch>")
add_dependencies(end_to_end_test mdbus_feed_handler mdbus_exchange_sim mdbus_watch)
set_tests_properties(end_to_end_test PROPERTIES PROCESSORS 3)  # the feed and the reader spin
add_mdbus_test(failure_test LABELS failure TIMEOUT 180)
# 300 ms per cell, 4 cells per round: about 8 s. A mutant that survives a round is run again, up
# to twice at 600 ms, before it fails the test.
add_mdbus_test(mutant_stress NO_MAIN LABELS stress ARGS 300 TIMEOUT 180)
# The exit rule itself: with a mutant that nothing can kill, mutant_stress must exit 1. Passes
# only on that exit after a clean control and at least one kill, where an any-kill rule would
# exit 0; a crash or an inconclusive control fails it. Not under TSan, which runs the control
# alone. The two never run at once: each spins four threads.
if(NOT MDBUS_SANITIZE STREQUAL "thread")
  add_test(NAME mutant_stress_refuses_a_survivor COMMAND mutant_stress 100 --with-equivalent-mutant)
  set_tests_properties(mutant_stress_refuses_a_survivor PROPERTIES LABELS stress
    PASS_REGULAR_EXPRESSION "Control clean, [1-5] of 6 mutants killed: exit 1\\.")
  set_tests_properties(mutant_stress mutant_stress_refuses_a_survivor
    PROPERTIES RESOURCE_LOCK stress_threads)
endif()

# Smoke: every benchmark briefly, one at a time (a bus run spins up to three P-cores).
# - A run that measured nothing exits non-zero. Failed gates do not fail a smoke run.
# - Each run makes its own directory under smoke/, so smoke/ itself must exist.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/smoke)
set(smoke --smoke 1 --duration-ms 200 --warmup-ms 100)
foreach(variant IN LISTS bus_variants)
  add_test(NAME smoke_${variant}  # paced writer and fast reader: hop and e2e
    COMMAND mdbus_bench_${variant} ${smoke} --rate 200000 --out smoke/${variant})
endforeach()
foreach(variant IN LISTS feed_variants)
  add_test(NAME smoke_${variant}  # one step of the book ladder
    COMMAND mdbus_bench_${variant} --smoke 1 --events 100000 --live 4096 --out smoke/${variant})
endforeach()
add_test(NAME smoke_dispatch  # virtual vs static dispatch, virtual side
  COMMAND mdbus_bench_dispatch ${smoke} --dispatch virtual --out smoke/dispatch)
add_test(NAME smoke_dispatch_static  # virtual vs static dispatch, static side
  COMMAND mdbus_bench_dispatch ${smoke} --dispatch static --out smoke/dispatch_static)
add_test(NAME smoke_Base_wsat_alone  # W-sat, no reader at all: the writer's own cost
  COMMAND mdbus_bench_Base ${smoke} --rate 0 --fast 0 --out smoke/wsat)
# The scenarios the comparisons run that the runs above do not. W-sat means unpaced: the writer
# publishes flat out.
add_test(NAME smoke_Base_wsat  # W-sat with a fast reader, stamp ring
  COMMAND mdbus_bench_Base ${smoke} --rate 0 --out smoke/wsat_fast)
add_test(NAME smoke_Mutex_wsat  # W-sat with a fast reader, mutex ring
  COMMAND mdbus_bench_Mutex ${smoke} --rate 0 --out smoke/mutex_wsat)
add_test(NAME smoke_Base_fanout  # slow reader on the hot ring (reads snapshots), stamp ring
  COMMAND mdbus_bench_Base ${smoke} --slow read --out smoke/fanout)
add_test(NAME smoke_Mutex_fanout  # slow reader on the hot ring, mutex ring
  COMMAND mdbus_bench_Mutex ${smoke} --slow read --out smoke/mutex_fanout)
get_property(all_tests DIRECTORY PROPERTY TESTS)
list(FILTER all_tests INCLUDE REGEX "^smoke_")
set_tests_properties(${all_tests} PROPERTIES LABELS "bench;smoke" TIMEOUT 120 RUN_SERIAL TRUE)
# The benchmark tooling (compare.py, report.py, the bench command lines and launcher), against the
# benchmarks above. Its compare.py cases need PyYAML, and are skipped without it.
add_test(NAME bench_tools_test
  COMMAND python3 ${PROJECT_SOURCE_DIR}/bench/bench_tools_test.py ${CMAKE_BINARY_DIR}/bench)
set_tests_properties(bench_tools_test PROPERTIES LABELS bench TIMEOUT 120 RUN_SERIAL TRUE)
