cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(PLATFORM STREQUAL "3ds")
        if(NOT DEFINED ENV{DEVKITPRO})
            message(FATAL_ERROR "Please define DEVKITPRO environment variable to point to your SDK path!")
        endif()
        set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/3DS.cmake" CACHE PATH "toolchain file")

    elseif(PLATFORM STREQUAL "vita")
        if(NOT DEFINED ENV{VITASDK})
            message(FATAL_ERROR "Please define VITASDK environment variable to point to your SDK path!")
        endif()
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    endif()
endif()

project(butterscotch C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra)
endif()

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

if(PLATFORM STREQUAL "ctr-cache-tool")
    add_definitions(-DHAVE_BZLIB_H=1)

    # Build the bzip2 library sources directly into the executable so the
    # resulting binary does not depend on a system libbz2.dll. Only the
    # library .c files - skip the bzip2/bzip2recover/test stubs.
    set(BZIP2_SOURCES
        vendor/bzip2/blocksort.c
        vendor/bzip2/bzlib.c
        vendor/bzip2/compress.c
        vendor/bzip2/crctable.c
        vendor/bzip2/decompress.c
        vendor/bzip2/huffman.c
        vendor/bzip2/randtable.c
    )

    add_executable(ctr-cache-preprocess
        tools/ctr_cache_preprocess.c
        tools/ctr_audio_preprocess.c     # audio bundle is part of the same tool
        tools/ctr_adpcm_encode.c         # DSP-ADPCM encoder for SFX
        src/binary_reader.c
        src/data_win.c
        src/3ds/ctr_texture_cache.c
        src/gl/image_decoder.c
        src/glfw/stb_impl.c
        ${BZIP2_SOURCES}
    )
    target_compile_definitions(ctr-cache-preprocess PRIVATE
        CTR_TEXTURE_CACHE_HOST
        IMAGE_DECODER_ENABLE_BZ2
    )
    target_include_directories(ctr-cache-preprocess PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/3ds
        ${CMAKE_SOURCE_DIR}/src/gl
        ${CMAKE_SOURCE_DIR}/vendor/bzip2
        ${CMAKE_SOURCE_DIR}/vendor/stb/ds
        ${CMAKE_SOURCE_DIR}/vendor/stb/image
        ${CMAKE_SOURCE_DIR}/vendor/stb/vorbis
    )
    target_link_libraries(ctr-cache-preprocess m)
    if(NOT MSVC)
        # stb_vorbis emits a wall of unused-but-set / sign-conversion warnings
        # we can't fix without forking the upstream single-header.
        set_source_files_properties(tools/ctr_audio_preprocess.c PROPERTIES
            COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-unused-function -Wno-sign-compare -Wno-unused-variable")
    endif()

    # Silence the slew of warnings the upstream bzip2 sources emit (unused
    # parameters, sign conversions etc.) - they are not our code to clean up.
    if(NOT MSVC)
        set_source_files_properties(${BZIP2_SOURCES} PROPERTIES
            COMPILE_FLAGS "-w")
    endif()

    # Static link on MinGW so the resulting .exe does not depend on
    # libwinpthread-1.dll / libgcc_s_*.dll / libstdc++-6.dll being on PATH.
    if(MINGW)
        target_link_options(ctr-cache-preprocess PRIVATE
            -static -static-libgcc -static-libstdc++)
    endif()

    return()
endif()


file(GLOB SOURCES src/*.c)
# Platform specific files builds
file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)

# OpenGL ES 3.0 / WebGL2 compatibility
option(ENABLE_GLES "Build the gl_renderer against OpenGL ES 3.0 / WebGL2 instead of desktop GL 4.1 Core" OFF)
if(PLATFORM STREQUAL "glfw" AND ENABLE_GLES)
    list(REMOVE_ITEM PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/glfw/gl_legacy_renderer.c)
endif()

add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES})
target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

# stb_ds
target_include_directories(butterscotch PUBLIC vendor/stb/ds)

if(PLATFORM STREQUAL "glfw")
    file(GLOB GL_SOURCES src/gl/*.c)
    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl)

    # Butterscotch VM/interpreter profiler
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" ON)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    # Butterscotch VM/interpreter tracing
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" ON)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    # Butterscotch VM opcode count profiler
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" ON)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    # Spatial grid logs
    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    # VM unknown/stubbed function logs
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" ON)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    # GLAD
    if(ENABLE_GLES)
        add_compile_definitions(ENABLE_GLES)
        add_library(glad STATIC vendor/glad-gles/src/glad.c)
        target_include_directories(glad PUBLIC vendor/glad-gles/include)
    else()
        add_library(glad STATIC vendor/glad/src/glad.c)
        target_include_directories(glad PUBLIC vendor/glad/include)
    endif()

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
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m opengl32 gdi32 winmm bz2)
        target_link_options(butterscotch PRIVATE -static)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread bz2)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread dl bz2)
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

    #gamecontrollerdb
    add_custom_command(TARGET butterscotch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/vendor/gamecontrollerdb.txt
            $<TARGET_FILE_DIR:butterscotch>/gamecontrollerdb.txt
    )
elseif(PLATFORM STREQUAL "ps2")
    # The PS2 EE has a single-precision FPU only, so double is software-emulated
    add_compile_definitions(USE_FLOAT_REALS)
    # By disabling Int64 support, we can fit the entire RValue struct in 8 bytes, improving performance and memory usage
    add_compile_definitions(NO_RVALUE_INT64)

    # Butterscotch VM/interpreter profiler
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" OFF)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    # Butterscotch VM/interpreter tracing
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" OFF)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    # Butterscotch VM opcode count profiler
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" OFF)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    # Spatial grid logs
    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    # VM unknown/stubbed function logs
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" OFF)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    # Platform identifier for PS2-specific code paths (e.g., scratchpad allocation)
    add_compile_definitions(PLATFORM_PS2)
    
    # PS2 renderer hot path logs
    option(ENABLE_PS2_RENDERER_LOGS "Enable hot path logs in the PS2 renderer" OFF)
    if(ENABLE_PS2_RENDERER_LOGS)
        add_compile_definitions(ENABLE_PS2_RENDERER_LOGS)
    endif()

    # PS2 audio system
    option(ENABLE_PS2_AUDIO "Enable the PS2 audio system" ON)
    if(ENABLE_PS2_AUDIO)
        add_compile_definitions(ENABLE_PS2_AUDIO)
    else()
        list(REMOVE_ITEM PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/ps2/ps2_audio_system.c)
    endif()

    # -fsingle-precision-constant: Treat unsuffixed float constants as single-precision to avoid accidental  float->double promotions on platforms with no hardware double
    # -Werror=double-promotion: Warn when something is being promoted to a double
    # -fomit-frame-pointer: frees a register on a target with tight register pressure
    # -ffast-math: enables aggressive floating point optimizations
    target_compile_options(butterscotch PRIVATE -fsingle-precision-constant -Werror=double-promotion -fomit-frame-pointer -ffast-math)

    if(PS2)
        # Compiling on the Docker container
        target_include_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
        target_link_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(butterscotch gskit dmakit audsrv pad mc kbd patches kernel c m)

        # Embed ps2sdk IRX modules (freesio2, freepad) as C arrays via bin2c
        # This avoids loading from rom0: which has compatibility issues between fat/slim PS2
        set(IRX_DIR $ENV{PS2SDK}/iop/irx)
        set(IRX_GEN_DIR ${CMAKE_BINARY_DIR}/generated/irx)
        file(MAKE_DIRECTORY ${IRX_GEN_DIR})

        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/freesio2_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freesio2.irx ${IRX_GEN_DIR}/freesio2_irx.c freesio2_irx
            DEPENDS ${IRX_DIR}/freesio2.irx
            COMMENT "Embedding freesio2.irx"
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
            OUTPUT ${IRX_GEN_DIR}/freepad_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freepad.irx ${IRX_GEN_DIR}/freepad_irx.c freepad_irx
            DEPENDS ${IRX_DIR}/freepad.irx
            COMMENT "Embedding freepad.irx"
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
        if(ENABLE_PS2_AUDIO)
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

        target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/freesio2_irx.c ${IRX_GEN_DIR}/mcman_irx.c ${IRX_GEN_DIR}/mcserv_irx.c ${IRX_GEN_DIR}/freepad_irx.c ${IRX_GEN_DIR}/usbd_irx.c ${IRX_GEN_DIR}/ps2kbd_irx.c)
        if(ENABLE_PS2_AUDIO)
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
elseif(PLATFORM STREQUAL "vita")
    add_compile_definitions(USE_FLOAT_REALS)
    add_compile_definitions(NO_RVALUE_INT64)
    add_compile_definitions(PLATFORM_PSVITA)

    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" ON)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" OFF)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" OFF)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    option(ENABLE_VM_RESOLVE_LOGS "Log every var read/write that fails to resolve to an instance (very noisy on 3DS)" OFF)
    if(ENABLE_VM_RESOLVE_LOGS)
        add_compile_definitions(ENABLE_VM_RESOLVE_LOGS)
    endif()

    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" ON)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)

    target_link_libraries(butterscotch
            -Wl,--whole-archive pthread -Wl,--no-whole-archive
            m
            z
            stdc++
            vorbis
            vorbisfile
            ogg
            vitaGL
            vitashark
            soloud
            mathneon
            SceShaccCgExt
            taihen_stub
            kubridge_stub
            SceAppMgr_stub
            SceAppUtil_stub
            SceAudio_stub
            SceAudioIn_stub
            SceCtrl_stub
            SceCommonDialog_stub
            SceDisplay_stub
            SceKernelDmacMgr_stub
            SceRazorHud_stub
            SceRazorCapture_stub
            SceFios2_stub
            SceGxm_stub
            SceLibcBridge_stub
            SceShaccCg_stub
            SceSysmodule_stub
            ScePower_stub
            SceTouch_stub
            SceVshBridge_stub
            SceMotion_stub
            SceNpTrophy_stub
    )
elseif(PLATFORM STREQUAL "3ds")
    add_compile_definitions(USE_FLOAT_REALS)
    add_compile_definitions(NO_RVALUE_INT64)
    add_compile_definitions(PLATFORM_3DS)

    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" ON)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" OFF)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" OFF)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    option(ENABLE_VM_RESOLVE_LOGS "Log every var read/write that fails to resolve to an instance (very noisy on 3DS)" OFF)
    if(ENABLE_VM_RESOLVE_LOGS)
        add_compile_definitions(ENABLE_VM_RESOLVE_LOGS)
    endif()

    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" ON)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)

    set(SHADER_SRC "${CMAKE_SOURCE_DIR}/src/3ds/shaders/render2d.v.pica")
    ctr_add_shader_library(render2d_shader "${SHADER_SRC}")
    dkp_add_embedded_binary_library(3ds_shaders render2d_shader)
    target_include_directories(butterscotch PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/.dkp-generated/3ds_shaders")

    if(DEFINED ENV{DEVKITPRO})
        target_include_directories(butterscotch PRIVATE
            $ENV{DEVKITPRO}/libctru/include
            $ENV{DEVKITPRO}/portlibs/3ds/include
        )
        target_link_directories(butterscotch PRIVATE
            $ENV{DEVKITPRO}/libctru/lib
            $ENV{DEVKITPRO}/portlibs/3ds/lib
        )
    endif()

    target_link_libraries(butterscotch
        SDL_mixer
        openal
        stdc++
        mad
        vorbisidec
        ogg
        mikmod
        SDL
        citro3d
        ctru
        bz2
        m
        3ds_shaders
    )

    target_compile_options(butterscotch PRIVATE
            -mcpu=arm1176jzf-s
            -mtune=arm1176jzf-s
            -marm
            -mfpu=vfpv2
            -mfloat-abi=hard

            -fno-strict-aliasing
            -fno-crossjumping

            -falign-functions=32
            -falign-loops=32
            -falign-jumps=32

            -ffast-math
            -fsingle-precision-constant
            -fno-math-errno
            -fno-trapping-math

            -ffunction-sections
            -fdata-sections
    )

    target_link_options(butterscotch PRIVATE
            -Wl,--gc-sections
    )

    #add_custom_target()

    ctr_create_3dsx(butterscotch)

    set(APP_TITLE "Butterscotch")
    set(APP_DESCRIPTION "GMS1.4-2.0 runner for 3DS")
    set(APP_AUTHOR "efimandreev0, Pugemon")
    set(APP_PRODUCT_CODE "CTR-P-BTRS")
    set(APP_UNIQUE_ID "0xEFADR")
    set(APP_VERSION_MAJOR "0")
    set(APP_VERSION_MINOR "1")
    set(APP_VERSION_MICRO "0")

    set(APP_RSF "${CMAKE_SOURCE_DIR}/3ds/template.rsf")
    set(APP_ICON "${CMAKE_SOURCE_DIR}/3ds/icon.png")
    set(BANNER_IMAGE "${CMAKE_SOURCE_DIR}/3ds/banner.png")
    set(BANNER_AUDIO "${CMAKE_SOURCE_DIR}/3ds/efimandreev0.wav")

    find_program(BANNERTOOL NAMES bannertool HINTS "$ENV{DEVKITPRO}/tools/bin")
    find_program(MAKEROM NAMES makerom HINTS "$ENV{DEVKITPRO}/tools/bin")

    if(BANNERTOOL AND MAKEROM)
        add_custom_command(
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.smdh"
                COMMAND ${BANNERTOOL} makesmdh
                -s "${APP_TITLE}"
                -l "${APP_DESCRIPTION}"
                -p "${APP_AUTHOR}"
                -i "${APP_ICON}"
                -o "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.smdh" 
                DEPENDS "${APP_ICON}"
                COMMENT "Building SMDH icon..."
        )

        add_custom_command(
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.bnr"
                COMMAND ${BANNERTOOL} makebanner
                -i "${BANNER_IMAGE}"
                -a "${BANNER_AUDIO}"
                -o "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.bnr" 
                DEPENDS "${BANNER_IMAGE}" "${BANNER_AUDIO}"
                COMMENT "Building Banner..."
        )

        add_custom_command(
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.cia"
                COMMAND ${MAKEROM} -f cia -o "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.cia"
                -DAPP_ENCRYPTED=false
                -rsf "${APP_RSF}"
                -target t
                -exefslogo
                -elf "$<TARGET_FILE:butterscotch>"
                -icon "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.smdh"
                -banner "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.bnr"
                -DAPP_TITLE="${APP_TITLE}"
                -DAPP_PRODUCT_CODE="${APP_PRODUCT_CODE}"
                -DAPP_UNIQUE_ID="${APP_UNIQUE_ID}"
                -DAPP_SYSTEM_MODE="80MB"
                -DAPP_SYSTEM_MODE_EXT="124MB"
                -major "${APP_VERSION_MAJOR}"
                -minor "${APP_VERSION_MINOR}"
                -micro "${APP_VERSION_MICRO}"
                DEPENDS butterscotch
                "${APP_RSF}"
                "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.smdh"
                "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.bnr"
                COMMENT "Building butterscotch.cia..."
        )

        add_custom_target(butterscotch_cia
                DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/butterscotch.cia"
        )

    else()
        message(WARNING "bannertool or makerom not found in devkitPro/tools/bin! CIA generation target is disabled.")
    endif()
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
