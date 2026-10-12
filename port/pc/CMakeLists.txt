cmake_minimum_required(VERSION 3.20)
project(BattleTanxGA_PC LANGUAGES C CXX)

option(BTGA_BUILD_RECOMP "Compile locally generated N64Recomp output" OFF)
set(BTGA_N64_RUNTIME_ROOT "${CMAKE_SOURCE_DIR}/../../.deps/N64ModernRuntime" CACHE PATH
    "Path to an N64ModernRuntime checkout")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(btga_pc_core
    src/game.cpp
    src/lzari.cpp
    src/rom.cpp
    src/sha256.cpp
    src/world.cpp
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

if(BTGA_BUILD_RECOMP)
    set(BTGA_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recomp/generated/functions")
    set(BTGA_RECOMP_HEADER "${BTGA_RECOMP_DIR}/funcs.h")
    set(BTGA_RECOMP_INCLUDE "${BTGA_N64_RUNTIME_ROOT}/N64Recomp/include")

    if(NOT EXISTS "${BTGA_RECOMP_HEADER}")
        message(FATAL_ERROR
            "Generated recompilation output is missing. Run port/pc/recomp/prepare.sh first.")
    endif()
    if(NOT EXISTS "${BTGA_RECOMP_INCLUDE}/recomp.h")
        message(FATAL_ERROR
            "N64ModernRuntime is missing at ${BTGA_N64_RUNTIME_ROOT}. "
            "Bootstrap the pinned runtime before enabling BTGA_BUILD_RECOMP.")
    endif()

    file(GLOB BTGA_RECOMP_C CONFIGURE_DEPENDS "${BTGA_RECOMP_DIR}/funcs_*.c")
    add_library(btga_recompiled_game STATIC
        ${BTGA_RECOMP_C}
        "${BTGA_RECOMP_DIR}/lookup.cpp"
    )
    target_include_directories(btga_recompiled_game PRIVATE
        "${BTGA_RECOMP_DIR}"
        "${BTGA_RECOMP_INCLUDE}"
    )
    target_compile_options(btga_recompiled_game PRIVATE
        $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-fno-strict-aliasing;-Wno-unused-variable>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-strict-aliasing;-Wno-unused-variable>
    )
endif()
