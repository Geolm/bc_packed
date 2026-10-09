cmake_minimum_required(VERSION 3.25)
project(bc_packed)

if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_MACOSX_BUNDLE TRUE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
endif()

set(CMAKE_C_STANDARD 11)
if(MSVC)
    set(CMAKE_C_FLAGS_DEBUG "/Zi /Od /DDEBUG /W4")
    set(CMAKE_C_FLAGS_RELEASE "/O2 /W4")
else()
    set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG -Wpedantic -Werror -Wall -Wextra -Wstrict-prototypes -Wgnu-empty-initializer -Wnewline-eof")
    set(CMAKE_C_FLAGS_RELEASE "-O3 -Wpedantic -Werror -Wall -Wextra -Wstrict-prototypes -Wgnu-empty-initializer -Wnewline-eof")
endif()

add_executable(unit_tests
    ./src/bc_packed.c
    ./tests/benchmark_test.c
    ./tests/main_test.c
    ./tests/synthetic_tests.c
)

if(NOT WIN32)
    target_compile_definitions(unit_tests PRIVATE HAS_IMAGE_TESTS)
    target_sources(unit_tests PRIVATE ./tests/image_tests.c)
endif()

target_include_directories(unit_tests PRIVATE ./src ./third_party)

if(APPLE)
    # Metal is only available on Apple platforms, so the GPU tests are macOS-only.
    # Exposed to the C runner via a compile definition.
    target_compile_definitions(unit_tests PRIVATE HAS_GPU_TESTS)
    # The .cpp is compiled as C++17 (CMake drives C++ sources with CXX_STANDARD).
    set_property(TARGET unit_tests PROPERTY CXX_STANDARD 17)
    # Mirror the C warning flags for C++, minus the C-only diagnostics
    # (-Wstrict-prototypes, -Wgnu-empty-initializer have no C++ meaning).
    target_compile_options(unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic -Werror -Wall -Wextra -Wnewline-eof>)
    # The vendored Metal.hpp uses anonymous structs, which trip -Wgnu-anonymous-struct.
    set_property(SOURCE ./tests/gpu_tests.cpp PROPERTY COMPILE_OPTIONS -Wno-gnu-anonymous-struct)
    target_sources(unit_tests PRIVATE ./tests/gpu_tests.cpp)
    target_link_libraries(unit_tests PRIVATE "-framework Metal" "-framework Foundation")
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(test m)
endif()