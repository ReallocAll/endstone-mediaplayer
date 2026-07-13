cmake_minimum_required(VERSION 3.21)

project(endstone_mediaplayer LANGUAGES C)

set(MP_VERSION "2.1.0" CACHE STRING "MediaPlayer plugin version")

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
    src/music_player.c
    src/sfunc.c
    third_party/cppcompat/src/common/alloc.c
    third_party/nbsparser/nbsparser.c
    third_party/stb/stb_ds_impl.c)

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
target_compile_definitions(endstone_mediaplayer PRIVATE MP_VERSION="${MP_VERSION}")
target_include_directories(endstone_mediaplayer PRIVATE
    include
    third_party/cppcompat/include
    third_party/cppcompat/src
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
else()
    target_compile_options(endstone_mediaplayer PRIVATE
        -fvisibility=hidden -fno-lto)
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
add_test(NAME nbs_parser COMMAND test_nbs_parser)
