cmake_minimum_required(VERSION 3.25)
project(bc1_packed)

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
    ./src/bc1_packed.c
    ./tests/benchmark_test.c
    ./tests/image_tests.c
    ./tests/main_test.c
    ./tests/synthetic_tests.c
)

target_include_directories(unit_tests PRIVATE ./src ./third_party)

if(UNIX AND NOT APPLE)
    target_link_libraries(test m)
endif()