cmake_minimum_required(VERSION 3.21)

project(endstone_mediaplayer LANGUAGES C)

set(MP_VERSION "2.1.1" CACHE STRING "MediaPlayer plugin version")
option(ENABLE_MPV_DEBUG_COMMANDS "Expose /mpv debug diagnostics" OFF)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "MediaPlayer supports x86-64 only")
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "MediaPlayer requires Clang")
endif()

if(CMAKE_C_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "MediaPlayer requires Clang 16 or newer for C23")
endif()

if(WIN32)
    if(NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(FATAL_ERROR "Windows builds require clang-cl")
    endif()
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Only Windows and Linux are supported")
endif()

set(MP_SOURCES
    src/plugin.c
    src/endstone_api.c
    src/music/music_catalog.c
    src/music/music_cache.c
    src/music/music_session.c
    src/music/music_commands.c
    src/sfunc.c
    src/screen/screen_geometry.c
    src/screen/screen_registry.c
    src/screen/screen_persistence.c
    src/video/video_format.c
    src/video/video_session.c
    src/video/video_catalog.c
    src/video/video_policy.c
    src/video/video_args.c
    src/video/video_commands.c
    src/bedrock/world_read_abi.c
    src/bedrock/world_write_abi.c
    src/map/map_abi.c
    src/map/map_render.c
    third_party/cppcompat/src/common/alloc.c
    third_party/cjson/cJSON.c
    third_party/miniz/miniz.c
    third_party/nbsparser/nbsparser.c
    third_party/stb/stb_ds_impl.c)

# miniz is stripped to the in-memory deflate/inflate/CRC core; the
# zlib-compatible names are disabled so nothing collides with a system
# zlib that another translation unit might pull in.
set(MP_MINIZ_DEFINES
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_STDIO
    MINIZ_NO_TIME
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

if(WIN32)
    list(APPEND MP_SOURCES
        third_party/cppcompat/src/msvc/string.c
        third_party/cppcompat/src/msvc/vector.c)
else()
    list(APPEND MP_SOURCES
        third_party/cppcompat/src/libcxx/string.c
        third_party/cppcompat/src/libcxx/vector.c)
endif()

add_library(endstone_mediaplayer SHARED ${MP_SOURCES})
target_compile_definitions(endstone_mediaplayer PRIVATE
    MP_VERSION="${MP_VERSION}"
    ${MP_MINIZ_DEFINES})
if(ENABLE_MPV_DEBUG_COMMANDS)
    target_compile_definitions(endstone_mediaplayer PRIVATE
        ENABLE_MPV_DEBUG_COMMANDS=1)
endif()
target_include_directories(endstone_mediaplayer PRIVATE
    include
    third_party/cppcompat/include
    third_party/cppcompat/src
    third_party/cjson
    third_party/miniz
    third_party/nbsparser
    third_party/stb)
set_target_properties(endstone_mediaplayer PROPERTIES
    PREFIX "endstone_"
    OUTPUT_NAME "mediaplayer")

if(WIN32)
    target_compile_definitions(endstone_mediaplayer PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    set_property(TARGET endstone_mediaplayer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    # Quote the expansions: an undefined variable would otherwise compare as
    # its own literal name and trip this guard on a clean configure.
    if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND
       NOT "${CMAKE_MSVC_RUNTIME_LIBRARY}" STREQUAL "" AND
       NOT "${CMAKE_MSVC_RUNTIME_LIBRARY}" STREQUAL "MultiThreadedDLL")
        message(FATAL_ERROR
            "MediaPlayer requires the dynamic UCRT (MultiThreadedDLL): it "
            "shares a heap with endstone_runtime.dll. Refusing the requested "
            "CMAKE_MSVC_RUNTIME_LIBRARY='${CMAKE_MSVC_RUNTIME_LIBRARY}'.")
    endif()
else()
    target_compile_options(endstone_mediaplayer PRIVATE
        -fvisibility=default -fno-lto -fno-omit-frame-pointer)
    target_link_options(endstone_mediaplayer PRIVATE
        -fuse-ld=lld -fno-lto -Wl,-z,defs)
    target_link_libraries(endstone_mediaplayer PRIVATE m)
endif()

set(PLUGINS_DIR "" CACHE PATH "Endstone plugins directory")
if(PLUGINS_DIR)
    install(TARGETS endstone_mediaplayer
        RUNTIME DESTINATION "${PLUGINS_DIR}"
        LIBRARY DESTINATION "${PLUGINS_DIR}")
endif()

# Sanitizer support (optional, off by default)
option(ENABLE_SANITIZER "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Unit tests
enable_testing()
add_executable(test_nbs_parser tests/test_nbs_parser.c
    third_party/nbsparser/nbsparser.c
    third_party/stb/stb_ds_impl.c)
target_include_directories(test_nbs_parser PRIVATE
    third_party/nbsparser
    third_party/stb)
target_compile_definitions(test_nbs_parser PRIVATE
    _CRT_SECURE_NO_WARNINGS)
set_target_properties(test_nbs_parser PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON)

if(ENABLE_SANITIZER)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_options(test_nbs_parser PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_nbs_parser PRIVATE
            -fsanitize=address,undefined)
    elseif(WIN32)
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" --print-resource-dir
            OUTPUT_VARIABLE CLANG_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        file(TO_CMAKE_PATH "${CLANG_RESOURCE_DIR}/lib/windows" CLANG_RUNTIME_DIR)

        set(ASAN_RUNTIME "${CLANG_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.lib")
        set(ASAN_THUNK "${CLANG_RUNTIME_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
        set(ASAN_DLL "${CLANG_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        foreach(runtime_file IN ITEMS "${ASAN_RUNTIME}" "${ASAN_THUNK}" "${ASAN_DLL}")
            if(NOT EXISTS "${runtime_file}")
                message(FATAL_ERROR "Required AddressSanitizer runtime not found: ${runtime_file}")
            endif()
        endforeach()

        # clang-cl's ASan does not support the debug CRT.  CMake also invokes
        # lld-link directly for Ninja builds, so link compiler-rt explicitly.
        set_property(TARGET test_nbs_parser PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        target_compile_options(test_nbs_parser PRIVATE
            /fsanitize=address)
        target_link_libraries(test_nbs_parser PRIVATE
            "${ASAN_RUNTIME}")
        target_link_options(test_nbs_parser PRIVATE
            "/WHOLEARCHIVE:${ASAN_THUNK}"
            /INCREMENTAL:NO)
        add_custom_command(TARGET test_nbs_parser POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ASAN_DLL}" "$<TARGET_FILE_DIR:test_nbs_parser>")
    endif()
endif()

add_test(NAME nbs_parser COMMAND test_nbs_parser)

# Video screen system tests
add_executable(test_video tests/test_video.c
    src/screen/screen_geometry.c
    src/screen/screen_registry.c
    src/screen/screen_persistence.c
    src/video/video_format.c
    src/video/video_session.c
    src/video/video_policy.c
    src/video/video_args.c
    src/bedrock/world_read_abi.c
    src/bedrock/world_write_abi.c
    src/map/map_abi.c
    src/map/map_render.c
    third_party/cjson/cJSON.c
    third_party/miniz/miniz.c)
target_include_directories(test_video PRIVATE
    include
    third_party/cppcompat/include
    third_party/cppcompat/src
    third_party/cjson
    third_party/miniz)
target_compile_definitions(test_video PRIVATE
    _CRT_SECURE_NO_WARNINGS
    ${MP_MINIZ_DEFINES})
if(ENABLE_MPV_DEBUG_COMMANDS)
    target_compile_definitions(test_video PRIVATE
        ENABLE_MPV_DEBUG_COMMANDS=1)
endif()
set_target_properties(test_video PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON)

if(WIN32)
    target_sources(test_video PRIVATE
        third_party/cppcompat/src/common/alloc.c
        third_party/cppcompat/src/msvc/string.c)
    set_property(TARGET test_video PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
else()
    target_sources(test_video PRIVATE
        third_party/cppcompat/src/common/alloc.c
        third_party/cppcompat/src/libcxx/string.c)
    target_link_libraries(test_video PRIVATE m)
endif()

if(ENABLE_SANITIZER)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_options(test_video PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_video PRIVATE
            -fsanitize=address,undefined)
    elseif(WIN32)
        set_property(TARGET test_video PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        target_compile_options(test_video PRIVATE /fsanitize=address)
        target_link_libraries(test_video PRIVATE "${ASAN_RUNTIME}")
        target_link_options(test_video PRIVATE
            "/WHOLEARCHIVE:${ASAN_THUNK}" /INCREMENTAL:NO)
        add_custom_command(TARGET test_video POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ASAN_DLL}" "$<TARGET_FILE_DIR:test_video>")
    endif()
endif()

add_test(NAME video_system COMMAND test_video)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME video_converter
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_convert_video.py")
endif()
