cmake_minimum_required(VERSION 3.14)
project(huv C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(LIBHUV_WARNINGS -Wall -Wextra -Wpedantic)

# HUV_SANITIZE — comma- or semicolon-separated list of -fsanitize=<…>
# sanitizers to enable project-wide. Empty (the default) disables them.
#
#   cmake -S . -B build-asan -DHUV_SANITIZE=address,undefined
#   cmake -S . -B build-msan -DHUV_SANITIZE=memory
#
# Flags are injected via add_compile_options + add_link_options BEFORE
# FetchContent_MakeAvailable, so the fetched third-party libraries
# (llhttp, mbedtls) are rebuilt with the same instrumentation as our
# own code. Without that, ASan/MSan would either miss violations
# inside a dep or (worse for MSan) produce noise from uninstrumented
# reads of memory the dep wrote.
#
# Note: `address` and `memory` are mutually exclusive (both hijack the
# allocator). `address,undefined` is the common and supported combo.
# `memory` is strict about deps — libc alone is not enough; see
# scripts/sanitize.sh for the caveat.
set(HUV_SANITIZE "" CACHE STRING
    "Sanitizers to enable (comma list): address, undefined, thread, memory")

if(HUV_SANITIZE)
    string(REPLACE "," ";" _san_list "${HUV_SANITIZE}")
    set(_san_flags "")
    foreach(s IN LISTS _san_list)
        list(APPEND _san_flags "-fsanitize=${s}")
    endforeach()
    # Frame pointers + -O1 give readable traces. We don't force -g here
    # because CMAKE_BUILD_TYPE=RelWithDebInfo (the recommended setting
    # for sanitizer builds) already supplies it.
    list(APPEND _san_flags -fno-omit-frame-pointer)
    add_compile_options(${_san_flags})
    add_link_options(${_san_flags})
    message(STATUS "Sanitizers enabled: ${HUV_SANITIZE}")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUV REQUIRED libuv)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(
    llhttp
    URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.2.1.tar.gz
)
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(BUILD_STATIC_LIBS ON CACHE INTERNAL "")
FetchContent_MakeAvailable(llhttp)

# mbedTLS — fetched release tarball includes the `framework/` submodule that
# a plain git archive lacks. Programs and tests are off; we only want the libs.
FetchContent_Declare(
    mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
)
set(ENABLE_PROGRAMS OFF CACHE INTERNAL "")
set(ENABLE_TESTING OFF CACHE INTERNAL "")
set(MBEDTLS_FATAL_WARNINGS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(mbedtls)

add_library(huv STATIC
    src/buf.c
    src/log.c
    src/async.c
    src/request.c
    src/response.c
    src/router.c
    src/conn.c
    src/tls.c
    src/server.c
)
target_compile_options(huv PRIVATE ${LIBHUV_WARNINGS})
target_include_directories(huv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${LIBUV_INCLUDE_DIRS}
)
target_link_libraries(huv
    PUBLIC
        ${LIBUV_LIBRARIES}
    PRIVATE
        llhttp_static
        mbedtls
        mbedx509
        mbedcrypto
)
target_link_directories(huv PUBLIC ${LIBUV_LIBRARY_DIRS})

# Self-signed cert for HTTPS demos/tests. Regenerated only if missing.
set(HUV_TLS_DIR ${CMAKE_BINARY_DIR}/tls)
set(HUV_TLS_CERT ${HUV_TLS_DIR}/server.crt)
set(HUV_TLS_KEY  ${HUV_TLS_DIR}/server.key)
find_program(OPENSSL_BIN openssl REQUIRED)
add_custom_command(
    OUTPUT ${HUV_TLS_CERT} ${HUV_TLS_KEY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${HUV_TLS_DIR}
    COMMAND ${OPENSSL_BIN} req -x509 -newkey rsa:2048 -sha256 -days 365 -nodes
            -keyout ${HUV_TLS_KEY} -out ${HUV_TLS_CERT}
            -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
    COMMENT "Generating self-signed TLS cert at ${HUV_TLS_DIR}"
    VERBATIM
)
add_custom_target(huv_tls_cert ALL DEPENDS ${HUV_TLS_CERT} ${HUV_TLS_KEY})

add_subdirectory(examples)
