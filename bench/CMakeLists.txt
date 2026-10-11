include(FetchContent)
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        192ef10025eb2c4cdd392bc502f0c852196baa48  # v1.9.5
  SYSTEM
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

add_executable(risk_benchmark risk_benchmark.cpp)
target_link_libraries(risk_benchmark PRIVATE oems_core oems_options benchmark::benchmark)

# Stage-by-stage cost of one order inside the gateway.
add_executable(pipeline_benchmark pipeline_benchmark.cpp)
target_link_libraries(pipeline_benchmark PRIVATE oems_gateway oems_options benchmark::benchmark)

# FIX load generator: runs against a live mini_oems.
add_executable(oems_load_client load_client.cpp)
target_link_libraries(oems_load_client PRIVATE oems_gateway oems_options)
