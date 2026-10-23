include_directories(${CMAKE_BINARY_DIR}/protocols
    ${CMAKE_SOURCE_DIR}/librt/libgracht/include ${CMAKE_SOURCE_DIR}/librt/libddk/include
    ${CMAKE_SOURCE_DIR}/librt/libds/include ${CMAKE_SOURCE_DIR}/librt/libc/include)
set(EDU_PROTOCOL ${CMAKE_BINARY_DIR}/protocols/ctt_driver_service_server.c)
set_source_files_properties(${EDU_PROTOCOL} PROPERTIES GENERATED TRUE)
add_module_target(edu-msi "" main.c ${EDU_PROTOCOL})
add_dependencies(edu-msi contract_servers)
deploy_file_to_initrd(edu-msi "modules" edu-msi.yaml)
