cmake_minimum_required(VERSION 3.20)
project(xbox_pearl_probe VERSION 0.1.0 LANGUAGES C CXX)

add_library(blake3_portable STATIC
  third_party/blake3/blake3.c
  third_party/blake3/blake3_dispatch.c
  third_party/blake3/blake3_portable.c)
target_include_directories(blake3_portable PUBLIC third_party/blake3)
target_compile_definitions(blake3_portable PRIVATE
  BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)
set_target_properties(blake3_portable PROPERTIES C_STANDARD 11)

find_package(Threads REQUIRED)
add_library(pearl_core STATIC core/pearl_core.cpp core/pearl_proof.cpp core/self_test.cpp core/work_queue.cpp)
target_include_directories(pearl_core PUBLIC core)
target_link_libraries(pearl_core PRIVATE blake3_portable Threads::Threads)
target_compile_features(pearl_core PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(pearl_core PRIVATE /W4 /WX)
else()
  target_compile_options(pearl_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(pearl_core_test tests/core_test.cpp)
target_link_libraries(pearl_core_test PRIVATE pearl_core)
enable_testing()
add_test(NAME pearl_reference_vectors COMMAND pearl_core_test)
add_executable(pearl_proof_fixture tests/proof_fixture.cpp)
target_link_libraries(pearl_proof_fixture PRIVATE pearl_core)
add_test(NAME pearl_proof_serialization COMMAND pearl_proof_fixture ${CMAKE_CURRENT_BINARY_DIR}/test-proofs)
add_executable(pearl_work_queue_test tests/work_queue_test.cpp)
target_link_libraries(pearl_work_queue_test PRIVATE pearl_core)
add_test(NAME pearl_parallel_preparation COMMAND pearl_work_queue_test)
set_tests_properties(pearl_parallel_preparation PROPERTIES TIMEOUT 30)
