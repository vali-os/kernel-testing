include(FetchContent)

# The buffer manager uses the real generated wire types, not hand-maintained
# test replicas. Keep host-test output separate from target protocol generation.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME mutex_waiter_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/mutex_waiter_test.py --mutation-check)
add_test(NAME amd64_stack_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/amd64_stack_test.py --mutation-check)
add_test(NAME firmware_boundary_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/firmware/test.sh)
add_test(NAME user_copy_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/user_copy_test.py)
add_test(NAME arm64_user_copy_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/arm64-kernel/test_user_copy.sh)
add_test(NAME arm64_memcpy_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/arm64-runtime/test_memcpy.sh)
add_test(NAME inheritance_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/inheritance_test.py)
add_test(NAME process_startup_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/process_startup_test.py --mutation-check)
add_test(NAME processd_exit_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/processd_exit_test.py --mutation-check)
add_test(NAME process_security_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/process_security_test.py)
add_test(NAME driver_identity_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/firmware/driver_identity_test.py --mutation-check)
add_test(NAME handle_provider_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/handle_provider_test.py)
add_test(NAME handle_provider_arch_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/handle_provider_arch_test.sh)
add_test(NAME handle_lifetime_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/handle_lifetime_test.py)
add_test(NAME grant_contract_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/grant_contract_test.py)
add_test(NAME rbtree_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/rbtree_test.py --mutation-check)
add_test(NAME libds_utf8_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/libds_utf8_test.py --mutation-check)
add_test(NAME dma_address_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_address_test.py)
add_test(NAME dma_iova_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_iova_test.py --mutation-check)
add_test(NAME dma_iommu_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_iommu_test.py --mutation-check)
add_test(NAME dma_iommu_bound_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_iommu_bound_test.py --mutation-check)
add_test(NAME arm_smmu_discovery_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/discovery_test.py --mutation-check)
add_test(NAME arm_smmu_devicetree_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/devicetree_test.py --mutation-check)
add_test(NAME arm_smmu_memory_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/memory_test.py --mutation-check)
add_test(NAME arm_smmu_stage2_table_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/stage2_table_test.py --mutation-check)
add_test(NAME arm_smmuv3_backend_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/v3/backend_test.py --mutation-check)
add_test(NAME arm_smmu500_backend_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/mmu500/backend_test.py --mutation-check)
add_test(NAME arm_smmu_mixed_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/mixed_test.py --mutation-check)
add_test(NAME arm_smmu_native_pci_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/native_pci_test.py --mutation-check)
add_test(NAME arm_smmu_boot_runner_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/smmu/boot_smmuv3.py --self-check)
add_test(NAME dma_bound_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_bound_test.py --mutation-check)
add_test(NAME dma_attachment_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_attachment_test.py --mutation-check)
add_test(NAME dma_service_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_service_test.py --mutation-check)
add_test(NAME dma_syscall_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_syscall_test.py)
add_test(NAME uhci_bounce_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/uhci_bounce_test.py)
add_test(NAME fs_shm_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/fs_shm_test.py)
add_test(NAME filed_storage_unregister_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/filed_storage_unregister_test.py --mutation-check)
add_test(NAME filed_storage_lifecycle_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/filed_storage_lifecycle_test.py --mutation-check)
add_test(NAME filed_storage_bind_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/filed_storage_bind_test.py --mutation-check)
add_test(NAME filed_storage_results_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/filed_storage_results_test.py --mutation-check)
add_test(NAME filed_vfs_move_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/filed_vfs_move_test.py --mutation-check)
add_test(NAME mfs_move_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/mfs_move_test.py --mutation-check)
add_test(NAME mfs_lock_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/mfs_lock_test.py --mutation-check)
add_test(NAME mfs_entries_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/mfs_entries_test.py --mutation-check)
add_test(NAME mfs_data_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/mfs_data_test.py --mutation-check)
add_test(NAME mfs_runs_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/mfs_runs_test.py --mutation-check)
add_test(NAME filed_driver_ops_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/filed_driver_ops_test.py --mutation-check)
add_test(NAME ddk_storage_retry_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ddk_storage_retry_test.py --mutation-check)
add_test(NAME ahci_dma_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ahci_dma_test.py)
add_test(NAME ddk_dma_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ddk_dma_test.py --mutation-check)
add_test(NAME ddk_dma_chunk_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ddk_dma_chunk_test.py --mutation-check)
add_test(NAME ddk_client_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ddk_client_test.py --mutation-check)
add_test(NAME ddk_time_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ddk_time_test.py --mutation-check)
add_test(NAME sdhci_adma_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sdhci_adma_test.py --mutation-check)
add_test(NAME log_format_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/log_format_test.py --mutation-check)
add_test(NAME kernel_log_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/kernel_log_test.py --mutation-check)
add_test(NAME syslogd_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/syslogd_test.py --mutation-check)
add_test(NAME ioset_ctrl_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ioset_ctrl_test.py --mutation-check)
add_test(NAME os_event_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/os_event_test.py --mutation-check)
add_test(NAME libc_event_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/libc_event_test.py --mutation-check)
add_test(NAME userevent_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/userevent_test.py --mutation-check)
add_test(NAME stdio_flush_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/stdio_flush_test.py --mutation-check)
add_test(NAME libc_null_stdio_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/libc_null_stdio_test.py --mutation-check)
add_test(NAME sd_card_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sd_card_test.py --mutation-check)
add_test(NAME sd_storage_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sd_storage_test.py --mutation-check)
add_test(NAME sd_slot_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sd_slot_test.py --mutation-check)
add_test(NAME sd_mmc_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sd_mmc_test.py --mutation-check)
add_test(NAME sdhci_controller_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sdhci_controller_test.py --mutation-check)
add_test(NAME sdhci_request_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sdhci_request_test.py --mutation-check)
add_test(NAME sdhci_bcm2711_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sdhci_bcm2711_test.py --mutation-check)
add_test(NAME sdhci_platform_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/sdhci_platform_test.py --mutation-check)
add_test(NAME rpi_firmware_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/rpi_firmware_test.py --mutation-check)
add_test(NAME ps2_mouse_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ps2_mouse_test.py --mutation-check)
add_test(NAME ps2_register_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ps2_register_test.py --mutation-check)
add_test(NAME os_buffer_access_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/os_buffer_access_test.py)
add_test(NAME dma_fence_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_fence_test.py)
add_test(NAME os_fence_shared_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/os_fence_shared_test.py)
add_test(NAME shm_device_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_device_test.py)
add_test(NAME shm_host_runner_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_host_runner_test.py)
add_test(NAME file_transfer_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/file_transfer_test.py)
add_test(NAME socket_io_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/socket_io_test.py --mutation-check)
add_test(NAME libc_socket_errno_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/libc_socket_errno_test.py --mutation-check)
add_test(NAME libc_file_transfer_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/libc_file_transfer_test.py)
add_test(NAME shm_client_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_client_test.py)
add_test(NAME shm_restricted_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_restricted_test.py)
add_test(NAME shm_creation_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_creation_test.py)
add_test(NAME usb_scheduler_memory_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/usb_scheduler_memory_test.py)
add_test(NAME xhci_mmio_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/xhci_mmio_test.sh)
add_test(NAME memory_region_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/memory_region_test.py)
add_test(NAME x86_device_pool_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/x86_device_pool_test.py --mutation-check)
add_test(NAME arm64_device_pool_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/arm64-kernel/test_device_pool.py --mutation-check)
add_test(NAME memory_stack_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/memory_stack_test.py)
add_test(NAME memory_boot_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/memory_boot_test.py)
add_test(NAME system_memory_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/system_memory_test.py)
add_test(NAME shm_memory_ownership_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_memory_ownership_test.py)
add_test(NAME interrupt_registration_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/testing/interrupt_registration_test.sh)
add_test(NAME netadapter_packet_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/netadapter/packet_test.py)
add_test(NAME netd_core_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/net/core_test.py)
add_test(NAME netd_core_tsan_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/net/core_test.py --tsan ring_test command_test bridge_thread_test stack_shutdown_test lifecycle_thread_test operation_test operation_output_test)
add_test(NAME netd_core_ubsan_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/net/core_test.py --ubsan)
add_test(NAME netd_stack_adapter_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/net/stack_adapter_test.py)
add_test(NAME netd_stack_adapter_tsan_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/net/stack_adapter_test.py --tsan)
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
        OUTPUT "${NET_BUFFER_PROTOCOL_DIR}/sys_process_service.h" "${NET_BUFFER_PROTOCOL_HEADER}" "${NET_BUFFER_PROTOCOL_DIR}/ctt_netadapter_service_server.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NET_BUFFER_PROTOCOL_DIR}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/librt/libgracht/generator/parser.py"
                --service "${CMAKE_SOURCE_DIR}/protocols/contracts/netadapter.gr"
                --out "${NET_BUFFER_PROTOCOL_DIR}" --lang-c --server
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/librt/libgracht/generator/parser.py"
                --service "${CMAKE_SOURCE_DIR}/protocols/process.gr"
                --out "${NET_BUFFER_PROTOCOL_DIR}" --lang-c
        DEPENDS ${CMAKE_SOURCE_DIR}/protocols/process.gr
                ${CMAKE_SOURCE_DIR}/protocols/contracts/netadapter.gr
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
        ${CMAKE_SOURCE_DIR}/librt/libgracht/include
        ${CMAKE_SOURCE_DIR}/librt/libddk/include)
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
        ${CMAKE_SOURCE_DIR}/librt/libgracht/include
        ${CMAKE_SOURCE_DIR}/librt/libddk/include)
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
        ${CMAKE_SOURCE_DIR}/librt/libos/include ${CMAKE_SOURCE_DIR}/librt/libgracht/include
        ${CMAKE_SOURCE_DIR}/librt/libddk/include)
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

add_test(NAME shm_region_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_region_test.py)
add_test(NAME shm_region_usage_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/shm_region_usage_test.py --mutation-check)
add_test(NAME dma_operation_gate_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_operation_gate_test.py --mutation-check)

add_test(NAME dma_region_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/dma_region_test.py --mutation-check)
add_test(NAME ddk_dma_region_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/testing/ddk_dma_region_test.py)
