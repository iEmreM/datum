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

add_library(datum_core
    src/analyze.cpp
    src/codecs.cpp
    src/container.cpp
    src/image.cpp
    src/io.cpp
    src/video.cpp
)
target_include_directories(datum_core PUBLIC include)
# SYSTEM: stb is third-party, its warnings must not trip our -Werror.
target_include_directories(datum_core SYSTEM PRIVATE third_party/stb)
target_compile_options(datum_core PRIVATE ${DATUM_WARNINGS})

add_executable(datum apps/datum.cpp)
target_link_libraries(datum PRIVATE datum_core)
target_compile_options(datum PRIVATE ${DATUM_WARNINGS})

# The Phase 5 robustness harness. Built with everything else so -Werror keeps it
# honest, but not registered as a test: it needs ffmpeg and its output is a table.
add_executable(datum_robustness tools/robustness.cpp)
target_link_libraries(datum_robustness PRIVATE datum_core)
target_include_directories(datum_robustness SYSTEM PRIVATE third_party/stb)
target_compile_options(datum_robustness PRIVATE ${DATUM_WARNINGS})

enable_testing()
add_executable(datum_tests tests/roundtrip.cpp)
target_link_libraries(datum_tests PRIVATE datum_core)
# The qim robustness check needs a real JPEG encoder; stb already is one.
target_include_directories(datum_tests SYSTEM PRIVATE third_party/stb)
target_compile_options(datum_tests PRIVATE ${DATUM_WARNINGS})
add_test(NAME roundtrip COMMAND datum_tests)

# A silently ignored flag once made `--bit 4` embed at the default depth and
# still report success. Guard the parser at the CLI level, where the bug lived.
add_test(NAME cli_rejects_unknown_flag
         COMMAND datum embed -i cover.png -o out.png -d payload.bin --bit 4)
set_tests_properties(cli_rejects_unknown_flag PROPERTIES
                     PASS_REGULAR_EXPRESSION "unknown flag --bit")
