include(FetchContent)

# The buffer manager uses the real generated wire types, not hand-maintained
# test replicas. Keep host-test output separate from target protocol generation.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME firmware_boundary_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/firmware/test.sh)
add_test(NAME arm64_memcpy_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/arm64-runtime/test_memcpy.sh)
add_test(NAME handle_lifetime_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/handle_lifetime_test.py)
add_test(NAME shm_device_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_device_test.py)
add_test(NAME interrupt_registration_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/interrupt_registration_test.sh)
add_test(NAME netadapter_packet_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/netadapter/packet_test.py)
add_test(NAME netadapter_registry_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/netadapter/registry_test.py)
add_test(NAME virtio_net_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/netadapter/virtio_test.py)
add_test(NAME virtio_net_session_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/netadapter/virtio_test.py --sessions-only)
add_test(NAME virtio_net_registry_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/netadapter/virtio_registry_test.sh)
add_test(NAME ipc_receive_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ipc_receive_test.py)
add_test(NAME netadapter_contract_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/netadapter_contract_test.py)
set(NET_BUFFER_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/netadapter-protocol")
set(NET_BUFFER_PROTOCOL_HEADER "${NET_BUFFER_PROTOCOL_DIR}/ctt_netadapter_service.h")
add_custom_command(
        OUTPUT "${NET_BUFFER_PROTOCOL_HEADER}" "${NET_BUFFER_PROTOCOL_DIR}/ctt_netadapter_service_server.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NET_BUFFER_PROTOCOL_DIR}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/librt/libgracht/generator/parser.py"
                --service "${CMAKE_SOURCE_DIR}/protocols/contracts/netadapter.gr"
                --out "${NET_BUFFER_PROTOCOL_DIR}" --lang-c --server
        DEPENDS ${CMAKE_SOURCE_DIR}/protocols/contracts/netadapter.gr
                ${CMAKE_SOURCE_DIR}/protocols/types.gr
                ${CMAKE_SOURCE_DIR}/librt/libgracht/generator/parser.py
                ${CMAKE_SOURCE_DIR}/librt/libgracht/generator/languages/langc.py
        VERBATIM)
add_custom_target(net_test_protocol DEPENDS "${NET_BUFFER_PROTOCOL_HEADER}"
        "${NET_BUFFER_PROTOCOL_DIR}/ctt_netadapter_service_server.h")
add_executable(net_buffers_test
        net_buffers_test.c
        net_shm_mock.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/buffers.c)
add_dependencies(net_buffers_test net_test_protocol)
target_compile_features(net_buffers_test PRIVATE c_std_11)
target_compile_definitions(net_buffers_test PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(net_buffers_test PRIVATE
        ${CMAKE_SOURCE_DIR}/testing/include
        ${CMAKE_SOURCE_DIR}/services/netd
        ${CMAKE_SOURCE_DIR}/services/netd/adapters
        ${CMAKE_SOURCE_DIR}/librt/libos/include
        ${CMAKE_SOURCE_DIR}/librt/libgracht/include)
target_include_directories(net_buffers_test SYSTEM PRIVATE "${NET_BUFFER_PROTOCOL_DIR}")
add_test(NAME net_buffers_test COMMAND net_buffers_test)

add_executable(net_adapter_test
        net_adapter_test.c net_shm_mock.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/adapter.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/rx.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/tx.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/queue.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/scheduler.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/events.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/buffers.c)
add_dependencies(net_adapter_test net_test_protocol)
target_compile_features(net_adapter_test PRIVATE c_std_11)
target_compile_definitions(net_adapter_test PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(net_adapter_test PRIVATE
        ${CMAKE_SOURCE_DIR}/services/netd/adapters
        ${CMAKE_SOURCE_DIR}/testing/include
        ${CMAKE_SOURCE_DIR}/services/netd
        ${CMAKE_SOURCE_DIR}/services/netd/adapters
        ${CMAKE_SOURCE_DIR}/librt/libos/include
        ${CMAKE_SOURCE_DIR}/librt/libgracht/include)
target_include_directories(net_adapter_test SYSTEM PRIVATE "${NET_BUFFER_PROTOCOL_DIR}")
add_test(NAME net_adapter_test COMMAND net_adapter_test)

# Same controller implementation as the guest service, driven by the real core.
add_executable(netadapter_fake_test
        netadapter/fake_test.c net_shm_mock.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/adapter.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/rx.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/tx.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/queue.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/scheduler.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/events.c
        ${CMAKE_SOURCE_DIR}/services/netd/adapters/buffers.c)
add_dependencies(netadapter_fake_test net_test_protocol)
target_compile_features(netadapter_fake_test PRIVATE c_std_11)
target_compile_definitions(netadapter_fake_test PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(netadapter_fake_test PRIVATE
        ${CMAKE_SOURCE_DIR}/testing ${CMAKE_SOURCE_DIR}/testing/include
        ${CMAKE_SOURCE_DIR}/services/netd ${CMAKE_SOURCE_DIR}/services/netd/adapters
        ${CMAKE_SOURCE_DIR}/librt/libos/include ${CMAKE_SOURCE_DIR}/librt/libgracht/include)
target_include_directories(netadapter_fake_test SYSTEM PRIVATE "${NET_BUFFER_PROTOCOL_DIR}")
add_test(NAME netadapter_fake_test COMMAND netadapter_fake_test)

FetchContent_Declare(
        cmocka
        GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
        GIT_TAG        f5e2cd77c88d9f792562888d2b70c5a396bfbf7a #cmocka-1.1.5
)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(WITH_STATIC_LIB OFF)
set(WITH_CMOCKERY_SUPPORT OFF)
set(WITH_EXAMPLES OFF)
set(UNIT_TESTING OFF)
set(PICKY_DEVELOPER OFF)
FetchContent_MakeAvailable(cmocka)

add_executable(hid_parser_test
        ${CMAKE_CURRENT_SOURCE_DIR}/hid_parser_test.c
        ${CMAKE_SOURCE_DIR}/modules/input/hid/report_parser.c)
target_include_directories(hid_parser_test PRIVATE
        ${CMAKE_SOURCE_DIR}/testing/include
        ${CMAKE_SOURCE_DIR}/modules/input/hid
        ${CMAKE_SOURCE_DIR}/librt/libddk/include
        ${CMAKE_SOURCE_DIR}/librt/libusb/include
        ${CMAKE_SOURCE_DIR}/librt/libos/include
        ${CMAKE_SOURCE_DIR}/librt/libds/include
        ${CMAKE_SOURCE_DIR}/boot/include)
target_link_libraries(hid_parser_test PRIVATE cmocka)
add_test(NAME hid_parser_test COMMAND hid_parser_test)

add_library(locale_test OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/locale_test.c
    ${CMAKE_SOURCE_DIR}/librt/libc/ctype/common/ctype_.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/duplocale.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/freelocale.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/lctype.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/lmessages.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/lmonetary.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/lnumeric.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/locale.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/localeconv.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/newlocale.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/timelocal.c
    ${CMAKE_SOURCE_DIR}/librt/libc/locale/uselocale.c
    ${CMAKE_SOURCE_DIR}/librt/libc/stdlib/getenv.c
    ${CMAKE_SOURCE_DIR}/librt/libc/stdlib/mb/mbtowc.c
    ${CMAKE_SOURCE_DIR}/librt/libc/stdlib/wide/wctomb.c)
target_compile_definitions(locale_test PRIVATE _MB_CAPABLE __OSLIB_C_IMPLEMENTATION)
target_include_directories(locale_test PRIVATE
        ${CMAKE_SOURCE_DIR}/librt/libc/include
        ${CMAKE_SOURCE_DIR}/librt/libddk/include
        ${CMAKE_SOURCE_DIR}/librt/libos/include)

configure_file(run-tests.sh ${CMAKE_BINARY_DIR}/run-tests.sh COPYONLY)
