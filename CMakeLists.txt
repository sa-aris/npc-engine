cmake_minimum_required(VERSION 3.16)
project(NPCBehaviorSystem
    VERSION 1.0.0
    DESCRIPTION "Comprehensive NPC Behavior System in C++17"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ─── NPC Library (static) ────────────────────────────────────────────

add_library(npc_lib STATIC
    src/npc.cpp
    src/world/world.cpp
)

target_include_directories(npc_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ─── Village Simulation Demo ─────────────────────────────────────────

add_executable(village_sim examples/village_sim.cpp)
target_link_libraries(village_sim PRIVATE npc_lib)

# ─── Test Suite ──────────────────────────────────────────────────────

add_executable(run_tests tests/run_tests.cpp)
target_include_directories(run_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(run_tests PRIVATE npc_lib)
target_compile_options(run_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-O2>
)

# CTest integration (optional: cmake --build build && ctest)
enable_testing()
add_test(NAME npc_tests COMMAND run_tests)
