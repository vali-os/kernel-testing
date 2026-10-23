include_directories(${CMAKE_BINARY_DIR}/protocols
    ${CMAKE_SOURCE_DIR}/librt/libgracht/include ${CMAKE_SOURCE_DIR}/librt/libddk/include
    ${CMAKE_SOURCE_DIR}/librt/libds/include ${CMAKE_SOURCE_DIR}/librt/libc/include)
set(ROCKER_PROTOCOL ${CMAKE_BINARY_DIR}/protocols/ctt_driver_service_server.c)
set_source_files_properties(${ROCKER_PROTOCOL} PROPERTIES GENERATED TRUE)
add_module_target(rocker-msix "" main.c ${ROCKER_PROTOCOL})
add_dependencies(rocker-msix contract_servers)
deploy_file_to_initrd(rocker-msix "modules" rocker-msix.yaml)
