cmake_minimum_required(VERSION 3.20)
project(BattleTanxGA_PC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(btga_pc_core
    src/game.cpp
    src/lzari.cpp
    src/rom.cpp
    src/sha256.cpp
)
target_include_directories(btga_pc_core PUBLIC include)
target_compile_options(btga_pc_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/WX>
)

add_executable(battletanx_ga_pc src/main.cpp)
target_link_libraries(battletanx_ga_pc PRIVATE btga_pc_core)

if(WIN32)
    target_sources(battletanx_ga_pc PRIVATE src/platform_win32.cpp)
    target_link_libraries(battletanx_ga_pc PRIVATE user32 gdi32)
else()
    find_package(X11 REQUIRED)
    target_sources(battletanx_ga_pc PRIVATE src/platform_x11.cpp)
    target_link_libraries(battletanx_ga_pc PRIVATE X11::X11)
endif()

enable_testing()
add_executable(btga_pc_tests tests/core_tests.cpp)
target_link_libraries(btga_pc_tests PRIVATE btga_pc_core)
add_test(NAME btga_pc_core COMMAND btga_pc_tests)
