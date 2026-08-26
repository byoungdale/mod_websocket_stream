cmake_minimum_required(VERSION 3.20)

project(mod_websocket_stream VERSION 0.0.1 LANGUAGES C)

include(CTest)

option(MWS_WARNINGS_AS_ERRORS "Treat this module's compiler warnings as errors" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# FreeSWITCH's Linux headers use POSIX/GNU interfaces such as strdup(). Keep
# strict C11 for this project while exposing the declarations those headers
# expect on glibc-based systems.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_GNU_SOURCE)
endif()

find_package(PkgConfig REQUIRED)

# Homebrew's curl is keg-only because macOS ships an older libcurl. Prefer the
# Homebrew .pc file when it is installed, without baking an architecture-specific
# /opt/homebrew path into the project.
if(APPLE)
    find_program(HOMEBREW_EXECUTABLE brew)
    if(HOMEBREW_EXECUTABLE)
        execute_process(
            COMMAND "${HOMEBREW_EXECUTABLE}" --prefix curl
            RESULT_VARIABLE HOMEBREW_CURL_RESULT
            OUTPUT_VARIABLE HOMEBREW_CURL_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(HOMEBREW_CURL_RESULT EQUAL 0)
            set(ENV{PKG_CONFIG_PATH}
                "${HOMEBREW_CURL_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        endif()
    endif()
endif()

pkg_check_modules(FREESWITCH REQUIRED IMPORTED_TARGET freeswitch>=1.11.0)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl>=8.21.0)
pkg_check_modules(SPEEXDSP REQUIRED IMPORTED_TARGET speexdsp>=1.2)
pkg_get_variable(FREESWITCH_MODULE_DIR freeswitch modulesdir)
pkg_get_variable(FREESWITCH_CONF_DIR freeswitch confdir)

file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_websocket_stream.c"
    MWS_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/captured_audio_buffer.c"
    MWS_CAPTURED_AUDIO_BUFFER_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/captured_audio_buffer.h"
    MWS_CAPTURED_AUDIO_BUFFER_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/captured_audio_resampler.c"
    MWS_CAPTURED_AUDIO_RESAMPLER_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/captured_audio_resampler.h"
    MWS_CAPTURED_AUDIO_RESAMPLER_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/bounded_stream.c"
    MWS_BOUNDED_STREAM_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/bounded_stream.h"
    MWS_BOUNDED_STREAM_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/freeswitch_command.c"
    MWS_FREESWITCH_COMMAND_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/freeswitch_command.h"
    MWS_FREESWITCH_COMMAND_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/http_headers.c"
    MWS_HTTP_HEADERS_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/http_headers.h"
    MWS_HTTP_HEADERS_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/resource_limits.h"
    MWS_RESOURCE_LIMITS_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/resource_limits.c"
    MWS_RESOURCE_LIMITS_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/playback_rate_controller.c"
    MWS_PLAYBACK_RATE_CONTROLLER_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/playback_rate_controller.h"
    MWS_PLAYBACK_RATE_CONTROLLER_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/playback_timing.c"
    MWS_PLAYBACK_TIMING_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/playback_timing.h"
    MWS_PLAYBACK_TIMING_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/playback_audio_buffer.c"
    MWS_PLAYBACK_AUDIO_BUFFER_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/playback_audio_buffer.h"
    MWS_PLAYBACK_AUDIO_BUFFER_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/playback_audio_resampler.c"
    MWS_PLAYBACK_AUDIO_RESAMPLER_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/playback_audio_resampler.h"
    MWS_PLAYBACK_AUDIO_RESAMPLER_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/source_audio_buffer.c"
    MWS_SOURCE_AUDIO_BUFFER_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/source_audio_buffer.h"
    MWS_SOURCE_AUDIO_BUFFER_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/websocket_protocol.c"
    MWS_WEBSOCKET_PROTOCOL_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/websocket_protocol.h"
    MWS_WEBSOCKET_PROTOCOL_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/websocket_audio_input.c"
    MWS_WEBSOCKET_AUDIO_INPUT_SOURCE_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/websocket_audio_input.h"
    MWS_WEBSOCKET_AUDIO_INPUT_HEADER_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" MWS_CMAKE_SHA256)
string(SHA256 MWS_BUILD_SHA256
    "${MWS_SOURCE_SHA256}:${MWS_BOUNDED_STREAM_SOURCE_SHA256}:${MWS_BOUNDED_STREAM_HEADER_SHA256}:${MWS_CAPTURED_AUDIO_BUFFER_SOURCE_SHA256}:${MWS_CAPTURED_AUDIO_BUFFER_HEADER_SHA256}:${MWS_CAPTURED_AUDIO_RESAMPLER_SOURCE_SHA256}:${MWS_CAPTURED_AUDIO_RESAMPLER_HEADER_SHA256}:${MWS_FREESWITCH_COMMAND_SOURCE_SHA256}:${MWS_FREESWITCH_COMMAND_HEADER_SHA256}:${MWS_HTTP_HEADERS_SOURCE_SHA256}:${MWS_HTTP_HEADERS_HEADER_SHA256}:${MWS_RESOURCE_LIMITS_SOURCE_SHA256}:${MWS_RESOURCE_LIMITS_HEADER_SHA256}:${MWS_PLAYBACK_RATE_CONTROLLER_SOURCE_SHA256}:${MWS_PLAYBACK_RATE_CONTROLLER_HEADER_SHA256}:${MWS_PLAYBACK_TIMING_SOURCE_SHA256}:${MWS_PLAYBACK_TIMING_HEADER_SHA256}:${MWS_PLAYBACK_AUDIO_BUFFER_SOURCE_SHA256}:${MWS_PLAYBACK_AUDIO_BUFFER_HEADER_SHA256}:${MWS_PLAYBACK_AUDIO_RESAMPLER_SOURCE_SHA256}:${MWS_PLAYBACK_AUDIO_RESAMPLER_HEADER_SHA256}:${MWS_SOURCE_AUDIO_BUFFER_SOURCE_SHA256}:${MWS_SOURCE_AUDIO_BUFFER_HEADER_SHA256}:${MWS_WEBSOCKET_AUDIO_INPUT_SOURCE_SHA256}:${MWS_WEBSOCKET_AUDIO_INPUT_HEADER_SHA256}:${MWS_WEBSOCKET_PROTOCOL_SOURCE_SHA256}:${MWS_WEBSOCKET_PROTOCOL_HEADER_SHA256}:${MWS_CMAKE_SHA256}")
string(SUBSTRING "${MWS_BUILD_SHA256}" 0 12 MWS_BUILD_ID)
file(WRITE "${CMAKE_BINARY_DIR}/module-build-id" "${MWS_BUILD_ID}\n")

add_library(mod_websocket_stream MODULE
    src/bounded_stream.c
    src/captured_audio_buffer.c
    src/captured_audio_resampler.c
    src/freeswitch_command.c
    src/http_headers.c
    src/playback_audio_buffer.c
    src/playback_audio_resampler.c
    src/playback_rate_controller.c
    src/playback_timing.c
    src/resource_limits.c
    src/source_audio_buffer.c
    src/websocket_audio_input.c
    src/websocket_protocol.c
    src/mod_websocket_stream.c
)

target_link_libraries(mod_websocket_stream PRIVATE
    PkgConfig::FREESWITCH
    PkgConfig::LIBCURL
    PkgConfig::SPEEXDSP
)

target_compile_definitions(mod_websocket_stream PRIVATE
    MWS_BUILD_ID="${MWS_BUILD_ID}"
)

target_compile_options(mod_websocket_stream PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
)

if(BUILD_TESTING AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    message(STATUS "Tests are not included in this source tree; skipping test targets")
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_executable(test_resource_limits
        tests/test_resource_limits.c
        src/resource_limits.c
    )
    target_include_directories(test_resource_limits PRIVATE src)
    target_compile_options(test_resource_limits PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME resource_limits
        COMMAND test_resource_limits
    )

    add_executable(test_bounded_stream
        tests/test_bounded_stream.c
        src/bounded_stream.c
    )
    target_include_directories(test_bounded_stream PRIVATE src)
    target_link_libraries(test_bounded_stream PRIVATE
        PkgConfig::FREESWITCH
    )
    target_compile_options(test_bounded_stream PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME bounded_stream
        COMMAND test_bounded_stream
    )

    add_executable(test_captured_audio_resampler
        tests/test_captured_audio_resampler.c
        src/captured_audio_resampler.c
        src/captured_audio_buffer.c
    )
    target_include_directories(test_captured_audio_resampler PRIVATE src)
    target_link_libraries(test_captured_audio_resampler PRIVATE
        PkgConfig::FREESWITCH
    )
    target_compile_options(test_captured_audio_resampler PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME captured_audio_resampler
        COMMAND test_captured_audio_resampler
    )

    add_executable(test_captured_audio_buffer
        tests/test_captured_audio_buffer.c
        src/captured_audio_buffer.c
    )
    target_include_directories(test_captured_audio_buffer PRIVATE src)
    target_link_libraries(test_captured_audio_buffer PRIVATE
        PkgConfig::FREESWITCH
    )
    target_compile_options(test_captured_audio_buffer PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME captured_audio_buffer
        COMMAND test_captured_audio_buffer
    )

    add_executable(test_freeswitch_command
        tests/test_freeswitch_command.c
        src/freeswitch_command.c
    )
    target_include_directories(test_freeswitch_command PRIVATE src)
    target_compile_options(test_freeswitch_command PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME freeswitch_command
        COMMAND test_freeswitch_command
    )

    add_executable(test_playback_rate_controller
        tests/test_playback_rate_controller.c
        src/playback_rate_controller.c
    )
    target_include_directories(test_playback_rate_controller PRIVATE src)
    target_compile_options(test_playback_rate_controller PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME playback_rate_controller
        COMMAND test_playback_rate_controller
    )

    add_executable(test_playback_audio_resampler
        tests/test_playback_audio_resampler.c
        src/playback_audio_resampler.c
        src/playback_rate_controller.c
        src/source_audio_buffer.c
    )
    target_include_directories(test_playback_audio_resampler PRIVATE src)
    target_link_libraries(test_playback_audio_resampler PRIVATE
        PkgConfig::FREESWITCH
        PkgConfig::SPEEXDSP
    )
    target_compile_options(test_playback_audio_resampler PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME playback_audio_resampler
        COMMAND test_playback_audio_resampler
    )

    add_executable(test_playback_timing
        tests/test_playback_timing.c
        src/playback_rate_controller.c
    )
    target_include_directories(test_playback_timing PRIVATE src)
    target_compile_options(test_playback_timing PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME playback_timing
        COMMAND test_playback_timing
    )

    add_executable(test_playback_timing_diagnostics
        tests/test_playback_timing_diagnostics.c
        src/playback_timing.c
    )
    target_include_directories(test_playback_timing_diagnostics PRIVATE src)
    target_compile_options(test_playback_timing_diagnostics PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME playback_timing_diagnostics
        COMMAND test_playback_timing_diagnostics
    )

    add_executable(test_source_audio_buffer
        tests/test_source_audio_buffer.c
        src/source_audio_buffer.c
    )
    target_include_directories(test_source_audio_buffer PRIVATE src)
    target_compile_options(test_source_audio_buffer PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME source_audio_buffer
        COMMAND test_source_audio_buffer
    )

    add_executable(test_playback_audio_buffer
        tests/test_playback_audio_buffer.c
        src/playback_audio_buffer.c
    )
    target_include_directories(test_playback_audio_buffer PRIVATE src)
    target_compile_options(test_playback_audio_buffer PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME playback_audio_buffer
        COMMAND test_playback_audio_buffer
    )

    add_executable(test_websocket_audio_input
        tests/test_websocket_audio_input.c
        src/websocket_audio_input.c
        src/source_audio_buffer.c
    )
    target_include_directories(test_websocket_audio_input PRIVATE src)
    target_link_libraries(test_websocket_audio_input PRIVATE
        PkgConfig::FREESWITCH
    )
    target_compile_options(test_websocket_audio_input PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME websocket_audio_input
        COMMAND test_websocket_audio_input
    )

    add_executable(test_websocket_protocol
        tests/test_websocket_protocol.c
        src/websocket_protocol.c
    )
    target_include_directories(test_websocket_protocol PRIVATE src)
    target_compile_options(test_websocket_protocol PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME websocket_protocol
        COMMAND test_websocket_protocol
    )

    add_executable(test_http_headers
        tests/test_http_headers.c
        src/http_headers.c
    )
    target_include_directories(test_http_headers PRIVATE src)
    target_compile_options(test_http_headers PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
        $<$<BOOL:${MWS_WARNINGS_AS_ERRORS}>:-Werror>
    )
    add_test(
        NAME http_headers
        COMMAND test_http_headers
    )
endif()

# FreeSWITCH modules use mod_name.so on both Linux and macOS.
set_target_properties(mod_websocket_stream PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mod_websocket_stream"
)

if(NOT FREESWITCH_MODULE_DIR)
    message(FATAL_ERROR "The freeswitch pkg-config file has no modulesdir variable")
endif()

install(TARGETS mod_websocket_stream
    LIBRARY DESTINATION "${FREESWITCH_MODULE_DIR}"
)
install(
    FILES conf/autoload_configs/websocket_stream.conf.xml
    DESTINATION "${FREESWITCH_CONF_DIR}/autoload_configs"
)
message(STATUS "FreeSWITCH version: ${FREESWITCH_VERSION}")
message(STATUS "FreeSWITCH module directory: ${FREESWITCH_MODULE_DIR}")
message(STATUS "libcurl version: ${LIBCURL_VERSION}")
message(STATUS "SpeexDSP version: ${SPEEXDSP_VERSION}")
message(STATUS "Module build ID: ${MWS_BUILD_ID}")
