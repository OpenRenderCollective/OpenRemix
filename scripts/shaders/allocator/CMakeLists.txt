# mimalloc for the Windows shader tools' C++ allocations (toolchain.py): spirv-opt makes
# millions of small allocations, which the Windows heap serves about 1.7x slower.
cmake_minimum_required(VERSION 3.16)
project(remix_shader_allocator C CXX)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
add_subdirectory(${MIMALLOC_SOURCE} mimalloc)
add_library(remix_shader_allocator STATIC new_delete.cpp)
target_link_libraries(remix_shader_allocator PRIVATE mimalloc-static)
