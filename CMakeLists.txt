cmake_minimum_required(VERSION 3.20)
project(datum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    set(DATUM_WARNINGS /W4 /WX /utf-8)
else()
    set(DATUM_WARNINGS -Wall -Wextra -Wpedantic -Werror)
endif()

# MinGW builds otherwise need libstdc++/libgcc/winpthread DLLs on PATH at run time.
# A single self-contained .exe is simpler to ship and to test.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static-libgcc -static-libstdc++ -static)
endif()

add_library(datum_core src/image.cpp)
target_include_directories(datum_core PUBLIC include)
# SYSTEM: stb is third-party, its warnings must not trip our -Werror.
target_include_directories(datum_core SYSTEM PRIVATE third_party/stb)
target_compile_options(datum_core PRIVATE ${DATUM_WARNINGS})

add_executable(datum apps/datum.cpp)
target_link_libraries(datum PRIVATE datum_core)
target_compile_options(datum PRIVATE ${DATUM_WARNINGS})

enable_testing()
add_executable(datum_tests tests/roundtrip.cpp)
target_link_libraries(datum_tests PRIVATE datum_core)
target_compile_options(datum_tests PRIVATE ${DATUM_WARNINGS})
add_test(NAME roundtrip COMMAND datum_tests)
