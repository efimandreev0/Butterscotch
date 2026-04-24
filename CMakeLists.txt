cmake_minimum_required(VERSION 3.21)
# Setup Nintendo Homebrew toolchains via devkitPro
if(PLATFORM STREQUAL "3ds" OR PLATFORM STREQUAL "wiiu")
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        if(NOT DEFINED ENV{DEVKITPRO})
            message(FATAL_ERROR "Please define DEVKITPRO environment variable to point to your SDK path!")
        endif()

        if(PLATFORM STREQUAL "3ds")
            set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/3DS.cmake" CACHE PATH "toolchain file")
        elseif(PLATFORM STREQUAL "wiiu")
            set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/WiiU.cmake" CACHE PATH "toolchain file")
        endif()
    endif()
endif()
project(butterscotch C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(PLATFORM "" CACHE STRING "Platform backend")

# Build metadata
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
    BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw (or another supported platform).")
endif()

# To avoid unused code in production builds that are only used for a single game (example: PlayStation 2 target), we can enable/disable specific bytecode versions
#
# If only a single bytecode version is enabled, the checks will be collapsed and removed by the compiler during build time, reducing code size and improving
# performance on low end hardware!
option(ENABLE_BC16 "Enable support for Bytecode Version 16" ON)
option(ENABLE_BC17 "Enable support for Bytecode Version 17" ON)
if(NOT ENABLE_BC16 AND NOT ENABLE_BC17)
    message(FATAL_ERROR "You need to build Butterscotch with at least one bytecode version enabled!")
endif()
if(ENABLE_BC16)
    add_compile_definitions(ENABLE_BC16)
endif()
if(ENABLE_BC17)
    add_compile_definitions(ENABLE_BC17)
endif()

file(GLOB SOURCES src/*.c)
# Platform specific files builds
file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)

add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES})
target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

# stb_ds
target_include_directories(butterscotch PUBLIC vendor/stb/ds)

if(PLATFORM STREQUAL "glfw")
    # GLAD
    add_library(glad STATIC vendor/glad/src/glad.c)
    target_include_directories(glad PUBLIC vendor/glad/include)

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # stb_vorbis
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)

    # miniaudio
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    if(MINGW)
        find_package(glfw3 REQUIRED)
        set(GLFW3_LIBRARIES glfw3)

        # Target Windows 7+ (avoids referencing newer APIs like MapViewOfFileNuma2 that MinGW import libs don't provide)
        # https://github.com/mirror/mingw-w64/blob/master/mingw-w64-headers/include/sdkddkver.h
        target_compile_definitions(butterscotch PRIVATE _WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000 WIN32_LEAN_AND_MEAN)
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m opengl32 gdi32 winmm)
        target_link_options(butterscotch PRIVATE -static)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread dl)
    endif()

    # Enable AddressSanitizer by default in Debug builds
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(ENABLE_ASAN_DEFAULT ON)
    else()
        set(ENABLE_ASAN_DEFAULT OFF)
    endif()
    set(ENABLE_ASAN ${ENABLE_ASAN_DEFAULT} CACHE BOOL "Enable AddressSanitizer")
    if(ENABLE_ASAN AND NOT MINGW AND NOT CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(butterscotch PRIVATE -fsanitize=address)
    endif()
elseif(PLATFORM STREQUAL "ps2")
    # The PS2 EE has a single-precision FPU only, so double is software-emulated
    add_compile_definitions(USE_FLOAT_REALS)
    # By disabling Int64 support, we can fit the entire RValue struct in 8 bytes, improving performance and memory usage
    add_compile_definitions(NO_RVALUE_INT64)
    # Disable Butterscotch VM/interpreter tracing checks
    add_compile_definitions(DISABLE_VM_TRACING)
    # Platform identifier for PS2-specific code paths (e.g., scratchpad allocation)
    add_compile_definitions(PLATFORM_PS2)
    # Disable the PS2 audio system
    option(DISABLE_PS2_AUDIO "Disable the PS2 audio system" OFF)
    if(DISABLE_PS2_AUDIO)
        add_compile_definitions(DISABLE_PS2_AUDIO)
        list(REMOVE_ITEM PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/ps2/ps2_audio_system.c)
    endif()
    # Treat unsuffixed float constants as single-precision to avoid accidental
    # float->double promotions on platforms with no hardware double
    add_compile_options(-fsingle-precision-constant)

    if(PS2)
        # Compiling on the Docker container
        target_include_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
        target_link_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(butterscotch gskit dmakit audsrv pad mc kbd patches kernel c m)

        # Embed ps2sdk IRX modules (sio2man, padman) as C arrays via bin2c
        # This avoids loading from rom0: which has compatibility issues between fat/slim PS2
        set(IRX_DIR $ENV{PS2SDK}/iop/irx)
        set(IRX_GEN_DIR ${CMAKE_BINARY_DIR}/generated/irx)
        file(MAKE_DIRECTORY ${IRX_GEN_DIR})

        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/sio2man_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/sio2man.irx ${IRX_GEN_DIR}/sio2man_irx.c sio2man_irx
            DEPENDS ${IRX_DIR}/sio2man.irx
            COMMENT "Embedding sio2man.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcman.irx ${IRX_GEN_DIR}/mcman_irx.c mcman_irx
            DEPENDS ${IRX_DIR}/mcman.irx
            COMMENT "Embedding mcman.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcserv_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcserv.irx ${IRX_GEN_DIR}/mcserv_irx.c mcserv_irx
            DEPENDS ${IRX_DIR}/mcserv.irx
            COMMENT "Embedding mcserv.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/padman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/padman.irx ${IRX_GEN_DIR}/padman_irx.c padman_irx
            DEPENDS ${IRX_DIR}/padman.irx
            COMMENT "Embedding padman.irx"
        )
        # We need the USB mass storage IRX modules for writing gmon.out when not running from host: and for keyboard support
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/usbd_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/usbd.irx ${IRX_GEN_DIR}/usbd_irx.c usbd_irx
            DEPENDS ${IRX_DIR}/usbd.irx
            COMMENT "Embedding usbd.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/ps2kbd_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/ps2kbd.irx ${IRX_GEN_DIR}/ps2kbd_irx.c ps2kbd_irx
            DEPENDS ${IRX_DIR}/ps2kbd.irx
            COMMENT "Embedding ps2kbd.irx"
        )
        if(NOT DISABLE_PS2_AUDIO)
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/freesd_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freesd.irx ${IRX_GEN_DIR}/freesd_irx.c freesd_irx
                DEPENDS ${IRX_DIR}/freesd.irx
                COMMENT "Embedding freesd.irx"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/audsrv_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/audsrv.irx ${IRX_GEN_DIR}/audsrv_irx.c audsrv_irx
                DEPENDS ${IRX_DIR}/audsrv.irx
                COMMENT "Embedding audsrv.irx"
            )
        endif()

        target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/sio2man_irx.c ${IRX_GEN_DIR}/mcman_irx.c ${IRX_GEN_DIR}/mcserv_irx.c ${IRX_GEN_DIR}/padman_irx.c ${IRX_GEN_DIR}/usbd_irx.c ${IRX_GEN_DIR}/ps2kbd_irx.c)
        if(NOT DISABLE_PS2_AUDIO)
            target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/freesd_irx.c ${IRX_GEN_DIR}/audsrv_irx.c)
        endif()
        target_include_directories(butterscotch PRIVATE ${IRX_GEN_DIR})

        # ===[ gprof Profiling Support ]===
        option(ENABLE_GPROF_PROFILING "Enable gprof profiling for PS2" OFF)
        if(ENABLE_GPROF_PROFILING)
            add_compile_definitions(GPROF_PROFILING)
            target_compile_options(butterscotch PRIVATE -pg)
            target_link_options(butterscotch PRIVATE -pg)

            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/bdm_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/bdm.irx ${IRX_GEN_DIR}/bdm_irx.c bdm_irx
                DEPENDS ${IRX_DIR}/bdm.irx
                COMMENT "Embedding bdm.irx (profiler)"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/bdmfs_fatfs.irx ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c bdmfs_fatfs_irx
                DEPENDS ${IRX_DIR}/bdmfs_fatfs.irx
                COMMENT "Embedding bdmfs_fatfs.irx (profiler)"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/usbmass_bd_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/usbmass_bd.irx ${IRX_GEN_DIR}/usbmass_bd_irx.c usbmass_bd_irx
                DEPENDS ${IRX_DIR}/usbmass_bd.irx
                COMMENT "Embedding usbmass_bd.irx (profiler)"
            )
            target_sources(butterscotch PRIVATE
                ${IRX_GEN_DIR}/bdm_irx.c
                ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c
                ${IRX_GEN_DIR}/usbmass_bd_irx.c
            )
        endif()
    else()
        # This block is for when we are developing Butterscotch with CLion!
        # ps2sdk and gsKit scatter headers across per-module subdirectories;
        # the flat include/ layout only exists after building the SDK.
        # We use file(GLOB) to discover all include dirs automatically.
        file(GLOB PS2SDK_EE_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/*/include)
        file(GLOB PS2SDK_EE_RPC_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/rpc/*/include)
        file(GLOB PS2SDK_EE_NETWORK_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/network/*/include)
        file(GLOB GSKIT_INCLUDES ${CMAKE_SOURCE_DIR}/gsKit/ee/*/include)

        # _EE is normally set by the PS2 toolchain; without it, tamtypes.h
        # refuses to define u64/u32/etc. We set it so CLion can resolve them.
        target_compile_definitions(butterscotch PRIVATE _EE)

        target_include_directories(butterscotch PRIVATE
                ${PS2SDK_EE_INCLUDES}
                ${PS2SDK_EE_RPC_INCLUDES}
                ${PS2SDK_EE_NETWORK_INCLUDES}
                ${CMAKE_SOURCE_DIR}/ps2sdk/common/include
                ${GSKIT_INCLUDES}
                # Newlib and GCC built-in headers are build artifacts (not in any repo),
                # so these are still extracted from Docker by setup.sh.
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/newlib-include
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/gcc-include
        )
    endif()
elseif(PLATFORM STREQUAL "3ds")
    # Nintendo 3DS target via devkitARM + libctru + citro3d + NovaGL (GLES1.1 translation layer).
    # Recommended to configure with:
    #   cmake -DPLATFORM=3ds -DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/3DS.cmake ...
    # and to have NOVAGL_DIR pointing to the NovaGL source tree (defaults to ../NovaGL).

    # The 3DS EE has double-precision FPU, but float is still ~2x faster — games generally
    # work fine with float reals and the memory savings on GMReal fit the 3DS' 128MB RAM.
    add_compile_definitions(USE_FLOAT_REALS)
    add_compile_definitions(NO_RVALUE_INT64)
    add_compile_definitions(DISABLE_VM_TRACING)
    add_compile_definitions(PLATFORM_3DS)

    # stb_image + stb_ds headers
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    #target_include_directories($ENV{DEVKITPRO}/portlibs/3ds/include)

    file(GLOB NOVAGL_SOURCES ${NOVAGL_DIR}/src/*.c)
    target_sources(butterscotch PRIVATE ${NOVAGL_SOURCES})
    target_include_directories(butterscotch PRIVATE ${NOVAGL_DIR}/include ${NOVAGL_DIR}/src)

    if(DEFINED ENV{DEVKITPRO})
        target_include_directories(butterscotch PRIVATE
            $ENV{DEVKITPRO}/libctru/include
            $ENV{DEVKITPRO}/portlibs/3ds/include)
        target_link_directories(butterscotch PRIVATE
            $ENV{DEVKITPRO}/libctru/lib
            $ENV{DEVKITPRO}/portlibs/3ds/lib)
    endif()

    # libctru defines __3DS__ via toolchain; we don't need to add it manually.
    target_link_libraries(butterscotch
            SDL_mixer
            mad
            vorbisidec
            ogg
            mikmod
            SDL
            NovaGL
            citro3d
            ctru m)

    # Aggressive optimization for the VM interpreter hot path (big rooms, battles).
    # -ffunction-sections/-fdata-sections + --gc-sections strips dead code, shrinking
    # the 3dsx and reducing ITCM pressure. -ffast-math is safe for game math (no NaN
    # handling needed). -fomit-frame-pointer frees r11 as a general-purpose register
    # on ARM — meaningful on the 3DS' register-starved A9.
    target_compile_options(butterscotch PRIVATE
        -O3
        -ffast-math
        -funroll-loops
        -fomit-frame-pointer
        -ffunction-sections
        -fdata-sections)
    target_link_options(butterscotch PRIVATE -Wl,--gc-sections)

    ctr_create_3dsx(butterscotch)
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
