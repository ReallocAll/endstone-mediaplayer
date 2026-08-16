cmake_minimum_required(VERSION 3.21)

project(endstone_mediaplayer LANGUAGES C)

# Keep the C and C++ consumer targets on the same clang-cl ABI on Windows.
# Production sources remain C; C++ is enabled only for the SDK compile check.
if(WIN32)
    set(CMAKE_CXX_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH
        "C++ compiler (must be clang-cl on Windows)" FORCE)
endif()
enable_language(CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "MediaPlayer SDK consumers require Clang C++")
endif()
if(WIN32 AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Windows SDK consumers require clang-cl")
endif()

set(MP_VERSION "2.3.1" CACHE STRING "MediaPlayer plugin version")
option(ENABLE_MPV_DEBUG_COMMANDS "Expose /mpv debug diagnostics" OFF)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake versions whose Clang-cl module predates C23 report only C17 even
# though the selected Clang supports the dialect.  Supply the known flag so
# C23 remains the actual production language mode when CXX is enabled.
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_C23_COMPILE_FEATURES c_std_23)
    set(CMAKE_C_STANDARD_LATEST 23)
    set(CMAKE_C23_STANDARD_COMPILE_OPTION "/clang:-std=c23")
    set(CMAKE_C23_EXTENSION_COMPILE_OPTION "/clang:-std=gnu23")
endif()

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
    src/api_provider.c
    src/endstone_api.c
    src/music/music_catalog.c
    src/music/music_cache.c
    src/music/music_sound.c
    src/music/music_session.c
    src/music/screen_audio.c
    src/music/music_commands.c
    src/sfunc.c
    src/screen/screen_geometry.c
    src/screen/screen_registry.c
    src/screen/screen_persistence.c
    src/screen/surface.c
    src/screen/presenter.c
    src/video/video_format.c
    src/video/video_session.c
    src/video/video_catalog.c
    src/video/video_policy.c
    src/video/video_preferences.c
    src/video/video_args.c
    src/video/video_commands.c
    src/bedrock/world_read_abi.c
    src/bedrock/world_write_abi.c
    src/map/map_abi.c
    src/map/map_render.c
    src/image/mps_format.c
    src/image/mps_catalog.c
    src/image/mps_source.c
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
    MP_PROVIDER_BUILD=1
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
    src/api_provider.c
    src/screen/screen_geometry.c
    src/screen/screen_registry.c
    src/screen/screen_persistence.c
    src/screen/surface.c
    src/screen/presenter.c
    src/video/video_format.c
    src/video/video_session.c
    src/video/video_policy.c
    src/video/video_preferences.c
    src/video/video_args.c
    src/music/music_cache.c
    src/music/music_catalog.c
    src/music/music_sound.c
    src/music/screen_audio.c
    src/bedrock/world_read_abi.c
    src/bedrock/world_write_abi.c
    src/map/map_abi.c
    src/map/map_render.c
    src/image/mps_format.c
    src/image/mps_catalog.c
    src/image/mps_source.c
    third_party/cjson/cJSON.c
    third_party/miniz/miniz.c)
target_sources(test_video PRIVATE
    third_party/nbsparser/nbsparser.c
    third_party/stb/stb_ds_impl.c)
target_include_directories(test_video PRIVATE
    include
    third_party/cppcompat/include
    third_party/cppcompat/src
    third_party/cjson
    third_party/miniz
    third_party/nbsparser
    third_party/stb)
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

# UTF-8 file-opening helper tests
add_executable(test_fopen_utf8 tests/test_fopen_utf8.c
    src/endstone_api.c
    third_party/cppcompat/src/common/alloc.c)
target_include_directories(test_fopen_utf8 PRIVATE
    include
    third_party/cppcompat/include
    third_party/cppcompat/src)
target_compile_definitions(test_fopen_utf8 PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(test_fopen_utf8 PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON)
if(WIN32)
    target_sources(test_fopen_utf8 PRIVATE
        third_party/cppcompat/src/msvc/string.c)
    set_property(TARGET test_fopen_utf8 PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL")
else()
    target_sources(test_fopen_utf8 PRIVATE
        third_party/cppcompat/src/libcxx/string.c)
endif()
add_test(NAME fopen_utf8 COMMAND test_fopen_utf8)

# Public SDK consumers.  They intentionally do not link the provider target;
# the provider is discovered only after loading the shared module at runtime.
add_library(mock_mediaplayer_provider SHARED
    tests/mock_mediaplayer_provider.c)
target_include_directories(mock_mediaplayer_provider PRIVATE include)
target_compile_definitions(mock_mediaplayer_provider PRIVATE MP_PROVIDER_BUILD=1)
set_target_properties(mock_mediaplayer_provider PROPERTIES
    PREFIX "endstone_"
    OUTPUT_NAME "mediaplayer"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mock_provider"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mock_provider"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mock_provider")
if(WIN32)
    set_property(TARGET mock_mediaplayer_provider PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
else()
    target_link_libraries(mock_mediaplayer_provider PRIVATE dl)
endif()

add_executable(test_sdk_c tests/test_sdk_c.c)
target_include_directories(test_sdk_c PRIVATE include)
set_target_properties(test_sdk_c PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON)
if(WIN32)
    set_property(TARGET test_sdk_c PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL")
else()
    target_link_libraries(test_sdk_c PRIVATE dl)
endif()
add_dependencies(test_sdk_c endstone_mediaplayer)
add_test(NAME sdk_c COMMAND test_sdk_c)

add_executable(test_sdk_cpp tests/test_sdk_cpp.cpp)
target_include_directories(test_sdk_cpp PRIVATE include)
set_target_properties(test_sdk_cpp PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
if(WIN32)
    set_property(TARGET test_sdk_cpp PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL")
else()
    target_link_libraries(test_sdk_cpp PRIVATE dl)
endif()
add_dependencies(test_sdk_cpp mock_mediaplayer_provider)
add_test(NAME sdk_cpp COMMAND test_sdk_cpp)

# Sparse surface transaction tests
add_executable(test_surface tests/test_surface.c
    src/screen/surface.c
    third_party/miniz/miniz.c)
target_include_directories(test_surface PRIVATE
    include
    third_party/miniz)
target_compile_definitions(test_surface PRIVATE
    _CRT_SECURE_NO_WARNINGS
    ${MP_MINIZ_DEFINES})
set_target_properties(test_surface PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON)
if(WIN32)
    set_property(TARGET test_surface PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL")
else()
    target_link_libraries(test_surface PRIVATE m)
endif()
add_test(NAME surface COMMAND test_surface)

# Sparse surface presenter scheduling tests
add_executable(test_presenter tests/test_presenter.c
    src/screen/presenter.c
    src/screen/surface.c
    src/screen/screen_geometry.c
    third_party/miniz/miniz.c)
target_include_directories(test_presenter PRIVATE
    include
    third_party/miniz)
target_compile_definitions(test_presenter PRIVATE
    _CRT_SECURE_NO_WARNINGS
    ${MP_MINIZ_DEFINES})
set_target_properties(test_presenter PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON)
if(WIN32)
    set_property(TARGET test_presenter PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL")
else()
    target_link_libraries(test_presenter PRIVATE m)
endif()
add_test(NAME presenter COMMAND test_presenter)

# Standalone MPS1 tiled image container tests
add_executable(test_mps tests/test_mps.c
    src/image/mps_format.c
    third_party/miniz/miniz.c)
target_include_directories(test_mps PRIVATE
    include
    third_party/miniz)
target_compile_definitions(test_mps PRIVATE
    _CRT_SECURE_NO_WARNINGS
    ${MP_MINIZ_DEFINES})
set_target_properties(test_mps PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON)
if(WIN32)
    set_property(TARGET test_mps PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL")
else()
    target_link_libraries(test_mps PRIVATE m)
endif()
add_test(NAME mps COMMAND test_mps)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME python_binding
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_binding.py"
                "${CMAKE_CURRENT_BINARY_DIR}/mock_provider")
    set_tests_properties(python_binding PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME video_converter
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_convert_video.py")
    add_test(NAME image_converter
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_convert_image.py")
endif()
