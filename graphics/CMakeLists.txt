cmake_minimum_required(VERSION 3.13.4)
project(vali-graphics-foundation C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The host transport fixture requires Linux abstract Unix sockets")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

get_filename_component(VALI_ROOT ../.. ABSOLUTE)
set(GRAPHICS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${GRAPHICS_GENERATED_DIR})

# Build the actual local Gracht runtime, including its current credential and
# codec behavior. No downloads or substitute RPC implementation are involved.
set(GRACHT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GRACHT_C_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(GRACHT_C_BUILD_SHARED ON CACHE BOOL "" FORCE)
add_subdirectory(${VALI_ROOT}/librt/libgracht ${CMAKE_CURRENT_BINARY_DIR}/gracht)
set(GRAPHICS_GRACHT_TARGET gracht_static)

file(GLOB GRAPHICS_GENERATOR_SOURCES CONFIGURE_DEPENDS
    ${VALI_ROOT}/librt/libgracht/generator/*.py
    ${VALI_ROOT}/librt/libgracht/generator/languages/*.py)
set(GRAPHICS_BINDINGS "")
foreach(CONTRACT graphics graphics_v3d graphics_test)
    set(CONTRACT_INPUT ${VALI_ROOT}/protocols/contracts/${CONTRACT}.gr)
    if(CONTRACT STREQUAL "graphics_test")
        set(CONTRACT_INPUT ${CMAKE_CURRENT_SOURCE_DIR}/graphics_test.gr)
    endif()
    set(OUTPUTS
        ${GRAPHICS_GENERATED_DIR}/ctt_${CONTRACT}_service.h
        ${GRAPHICS_GENERATED_DIR}/ctt_${CONTRACT}_service_client.h
        ${GRAPHICS_GENERATED_DIR}/ctt_${CONTRACT}_service_client.c
        ${GRAPHICS_GENERATED_DIR}/ctt_${CONTRACT}_service_server.h
        ${GRAPHICS_GENERATED_DIR}/ctt_${CONTRACT}_service_server.c)
    add_custom_command(OUTPUT ${OUTPUTS}
        COMMAND ${Python3_EXECUTABLE} ${VALI_ROOT}/librt/libgracht/generator/parser.py
            --service ${CONTRACT_INPUT}
            --out ${GRAPHICS_GENERATED_DIR} --lang-c --client --server
        DEPENDS ${CONTRACT_INPUT}
            ${VALI_ROOT}/protocols/contracts/graphics_types.gr
            ${VALI_ROOT}/protocols/types.gr ${GRAPHICS_GENERATOR_SOURCES})
    list(APPEND GRAPHICS_BINDINGS ${OUTPUTS})
endforeach()
add_custom_target(contract_servers DEPENDS ${GRAPHICS_BINDINGS})

# Use the real libos types with the project's existing host SAL/CRT definitions.
# Keeping libc headers off this list lets host sockets use their native ABI.
include_directories(${VALI_ROOT}/testing/include ${VALI_ROOT}/librt/libos/include)
add_subdirectory(${VALI_ROOT}/modules/graphics/common ${CMAKE_CURRENT_BINARY_DIR}/common)
# Exercise the real libos dependency and fence implementations. Only OS futex/TLS
# entry points use a host shim; clients never receive writable fence storage.
add_library(graphics-host-sync STATIC
    ${VALI_ROOT}/librt/libos/fence_pool.c
    ${VALI_ROOT}/librt/libos/buffer_access.c
    ${VALI_ROOT}/librt/libos/fence_storage.c sync_host.c)
target_compile_options(graphics-host-sync PRIVATE
    -idirafter ${VALI_ROOT}/librt/libc/include -Derrno_t=int)
target_link_libraries(graphics-core PUBLIC graphics-host-sync)
add_library(graphics-test-backend STATIC backend.c storage.c jobs.c)
target_link_libraries(graphics-test-backend PUBLIC graphics-core)

add_library(graphics-clients STATIC
    ${GRAPHICS_GENERATED_DIR}/ctt_graphics_service_client.c
    ${GRAPHICS_GENERATED_DIR}/ctt_graphics_v3d_service_client.c
    ${GRAPHICS_GENERATED_DIR}/ctt_graphics_test_service_client.c)
set_source_files_properties(
    ${GRAPHICS_GENERATED_DIR}/ctt_graphics_service_client.c
    ${GRAPHICS_GENERATED_DIR}/ctt_graphics_v3d_service_client.c
    ${GRAPHICS_GENERATED_DIR}/ctt_graphics_test_service_client.c
    ${GRAPHICS_GENERATED_DIR}/ctt_graphics_test_service_server.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-parameter;-Wno-sign-compare")
add_dependencies(graphics-clients contract_servers)
target_include_directories(graphics-clients SYSTEM PUBLIC ${GRAPHICS_GENERATED_DIR})
target_link_libraries(graphics-clients PUBLIC gracht_static Threads::Threads)

add_library(graphics-test-protocol STATIC requests.c
    ${GRAPHICS_GENERATED_DIR}/ctt_graphics_test_service_server.c)
add_dependencies(graphics-test-protocol contract_servers)
target_link_libraries(graphics-test-protocol PUBLIC graphics-common graphics-test-backend)

add_library(graphics-host-transport STATIC transport.c)
target_link_libraries(graphics-host-transport PUBLIC graphics-common graphics-clients)

add_executable(graphics-core-test core_test.c)
target_link_libraries(graphics-core-test PRIVATE graphics-test-backend)
add_test(NAME graphics-core COMMAND graphics-core-test)

add_executable(graphics-protocol-test protocol_test.c protocol_buffers_test.c)
target_link_libraries(graphics-protocol-test PRIVATE graphics-host-transport graphics-test-backend graphics-test-protocol)
add_test(NAME graphics-protocol COMMAND graphics-protocol-test)

add_executable(graphics-test-server server.c)
target_link_libraries(graphics-test-server PRIVATE graphics-host-transport graphics-test-backend graphics-test-protocol)
add_executable(graphics-test-client client.c)
target_link_libraries(graphics-test-client PRIVATE graphics-host-transport)
add_test(NAME graphics-process-clients
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/process_test.py
        $<TARGET_FILE:graphics-test-server> $<TARGET_FILE:graphics-test-client>)
set_tests_properties(graphics-core graphics-protocol graphics-process-clients PROPERTIES TIMEOUT 20)

foreach(TARGET graphics-core graphics-common graphics-test-backend graphics-clients graphics-test-protocol graphics-host-sync
    graphics-host-transport graphics-core-test graphics-protocol-test graphics-test-server graphics-test-client)
    target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Werror)
endforeach()
# Tests use assertions for both transport results and behavior; Release builds
# must still execute those expressions.
foreach(TARGET graphics-core-test graphics-protocol-test graphics-test-client graphics-test-server graphics-host-transport graphics-host-sync)
    target_compile_options(${TARGET} PRIVATE -UNDEBUG)
endforeach()

add_executable(graphics-buffer-test buffer_test.c)
target_link_libraries(graphics-buffer-test PRIVATE graphics-test-backend)
target_compile_options(graphics-buffer-test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME graphics-buffers COMMAND graphics-buffer-test)
set_tests_properties(graphics-buffers PROPERTIES TIMEOUT 20)
