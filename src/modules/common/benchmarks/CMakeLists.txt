find_package(benchmark CONFIG REQUIRED)

# Add more benchmark files here
set(BENCHMARKS
    spsc_ring_buffer.bench
)

foreach(b IN LISTS BENCHMARKS)
  add_executable(${b} ${b}.cpp)
  target_link_libraries(${b} PRIVATE benchmark::benchmark DTH::common)
endforeach()
