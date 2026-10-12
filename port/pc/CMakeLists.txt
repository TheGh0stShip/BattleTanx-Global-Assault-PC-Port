cmake_minimum_required(VERSION 3.20)
project(BattleTanxGA_PC LANGUAGES C CXX)

include(FetchContent)

set(BTGA_NET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(btga_networking
    GIT_REPOSITORY https://github.com/TheGh0stShip/BattleTanx-Global-Assault-Networking.git
    GIT_TAG 97b190991311525e360b426e4d303a948264c881
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(btga_networking)

option(BTGA_BUILD_RECOMP "Compile locally generated N64Recomp output" OFF)
option(BTGA_BUILD_RUNTIME_PROBE "Link the translated game against N64ModernRuntime" OFF)
option(BTGA_BUILD_RT64 "Build the optional RT64 renderer adapter" OFF)
set(BTGA_N64_RUNTIME_ROOT "${CMAKE_SOURCE_DIR}/../../.deps/N64ModernRuntime" CACHE PATH
    "Path to an N64ModernRuntime checkout")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(btga_pc_core
    src/controller_pak.cpp
    src/game.cpp
    src/lzari.cpp
    src/online_multiplayer.cpp
    src/pc_settings.cpp
    src/rom.cpp
    src/sha256.cpp
    src/texture_packs.cpp
    src/udp_online_io.cpp
    src/world.cpp
)
target_include_directories(btga_pc_core PUBLIC include)
target_link_libraries(btga_pc_core PUBLIC BattleTanxGA::NetProtocol)
if(WIN32)
    target_link_libraries(btga_pc_core PUBLIC ws2_32)
endif()
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
        $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-fno-strict-aliasing;-Wno-unused-variable;-include;${CMAKE_CURRENT_SOURCE_DIR}/recomp/runtime_compat.h>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-strict-aliasing;-Wno-unused-variable>
        $<$<COMPILE_LANG_AND_ID:C,MSVC>:/FI${CMAKE_CURRENT_SOURCE_DIR}/recomp/runtime_compat.h>
    )

    if(BTGA_BUILD_RUNTIME_PROBE)
        if(NOT EXISTS "${BTGA_N64_RUNTIME_ROOT}/CMakeLists.txt")
            message(FATAL_ERROR
                "N64ModernRuntime is missing at ${BTGA_N64_RUNTIME_ROOT}. "
                "Run port/pc/recomp/bootstrap_runtime.sh first.")
        endif()
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/recomp/generated/n_aspMain.cpp")
            message(FATAL_ERROR
                "Generated audio microcode is missing. Build RSPRecomp and rerun "
                "port/pc/recomp/prepare.sh.")
        endif()

        add_subdirectory("${BTGA_N64_RUNTIME_ROOT}" "${CMAKE_BINARY_DIR}/N64ModernRuntime")
        # N64ModernRuntime currently adds a GCC-style warning suppression
        # unconditionally. MSVC parses it as /W... and rejects the build.
        if(MSVC)
            set_property(TARGET ultramodern PROPERTY COMPILE_OPTIONS "")
            set_property(TARGET librecomp PROPERTY COMPILE_OPTIONS "")
            # MSVC supports the C99 constructs used by o1heap but does not
            # advertise __STDC_VERSION__ in C mode.
            target_compile_definitions(librecomp PRIVATE
                $<$<COMPILE_LANGUAGE:C>:__STDC_VERSION__=199901L>)
        endif()
        if(BTGA_BUILD_RT64)
            set(BTGA_RT64_ROOT "${CMAKE_SOURCE_DIR}/../../.deps/rt64" CACHE PATH "Path to RT64")
            find_package(SDL2 CONFIG REQUIRED)
            set(RT64_STATIC ON CACHE BOOL "" FORCE)
            set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "" FORCE)
            set(NFD_STUB ON CACHE BOOL "" FORCE)
            add_subdirectory("${BTGA_RT64_ROOT}" "${CMAKE_BINARY_DIR}/rt64")
            add_library(btga_rt64_renderer recomp/rt64_context.cpp)
            target_include_directories(btga_rt64_renderer PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/recomp"
                "${BTGA_RT64_ROOT}/src"
                "${BTGA_RT64_ROOT}/src/contrib"
                "${BTGA_RT64_ROOT}/src/contrib/hlslpp/include"
                "${BTGA_RT64_ROOT}/src/contrib/dxc/inc"
                "${BTGA_N64_RUNTIME_ROOT}/ultramodern/include"
            )
            target_link_libraries(btga_rt64_renderer PRIVATE rt64 ultramodern SDL2::SDL2)
            if(WIN32)
                target_link_libraries(btga_rt64_renderer PRIVATE shell32)
            endif()
            target_compile_definitions(btga_rt64_renderer PRIVATE
                RT64_SDL_WINDOW_VULKAN HLSL_CPU NOMINMAX)
        endif()
        add_executable(btga_runtime_probe
            recomp/controller_pak_hle.cpp
            recomp/generated/n_aspMain.cpp
            recomp/register_overlays.cpp
            recomp/runtime_probe.cpp
            recomp/runtime_compat.cpp
            recomp/runtime_patches.cpp
        )
        set_target_properties(btga_runtime_probe PROPERTIES OUTPUT_NAME "BattleTanxGA")
        target_include_directories(btga_runtime_probe PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${BTGA_RECOMP_DIR}"
            "${BTGA_N64_RUNTIME_ROOT}/librecomp/include"
            "${BTGA_N64_RUNTIME_ROOT}/N64Recomp/include"
            "${BTGA_N64_RUNTIME_ROOT}/thirdparty"
            "${BTGA_N64_RUNTIME_ROOT}/ultramodern/include"
        )
        target_link_libraries(btga_runtime_probe PRIVATE
            btga_pc_core
            btga_recompiled_game
            librecomp
            ultramodern
        )
        if(WIN32)
            target_link_libraries(btga_runtime_probe PRIVATE comdlg32 user32)
        endif()
        if(BTGA_BUILD_RT64)
            target_link_libraries(btga_runtime_probe PRIVATE btga_rt64_renderer SDL2::SDL2)
            target_compile_definitions(btga_runtime_probe PRIVATE BTGA_HAS_RT64)
            if(WIN32)
                target_link_directories(btga_runtime_probe PRIVATE
                    "${BTGA_RT64_ROOT}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64")
            endif()
        endif()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            target_compile_options(btga_runtime_probe PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:-msse4.1>
            )
        endif()
    endif()
endif()
