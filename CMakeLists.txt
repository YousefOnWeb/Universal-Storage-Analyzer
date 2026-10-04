cmake_minimum_required(VERSION 3.21)

project(UniversalStorageAnalyzer
    VERSION 0.1.0
    DESCRIPTION "Universal Storage Analyzer (FOSS) - cross-platform storage intelligence"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Strong warnings per AGENTS.md §4
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion)
endif()

# Allow callers to point at a Qt6 install, e.g.:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/tools/msys64/ucrt64"
list(APPEND CMAKE_PREFIX_PATH "C:/tools/msys64/ucrt64")

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Sql Test)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
