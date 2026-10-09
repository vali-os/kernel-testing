cmake_minimum_required(VERSION 3.20)
# This standalone fixture must never use host defaults or the OS build's NASM.
if(NOT CMAKE_C_COMPILER_TARGET STREQUAL "aarch64-uml-vali")
    message(FATAL_ERROR "Set CMAKE_C_COMPILER_TARGET=aarch64-uml-vali; Windows is only a control")
endif()
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
set(CMAKE_ASM_COMPILER_TARGET "${CMAKE_C_COMPILER_TARGET}")
project(vali-arm64-abi C ASM)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "These fixtures require the Vali Clang compiler")
endif()
add_compile_options(-march=armv8-a -mno-outline-atomics -ffixed-x18
                    -ffreestanding -nostdlibinc -fms-extensions)
add_library(abi-model OBJECT model.c)
target_compile_definitions(abi-model PRIVATE VALI_ABI_CONTRACT)
add_library(abi-features OBJECT features.c assembler.S)
add_library(abi-kernel OBJECT kernel.c)
target_compile_options(abi-kernel PRIVATE -mgeneral-regs-only)
# Explicit, small compiler-rt dependency set for the division fixture. This is
# not the complete production builtins archive. No host headers/libraries.
if(NOT EXISTS "${COMPILER_RT_SOURCE}/lib/builtins/divti3.c")
    message(FATAL_ERROR "Set COMPILER_RT_SOURCE to the LLVM compiler-rt source directory")
endif()
add_library(abi-builtins STATIC
    ${COMPILER_RT_SOURCE}/lib/builtins/divti3.c
    ${COMPILER_RT_SOURCE}/lib/builtins/udivmodti4.c)
target_compile_options(abi-builtins PRIVATE -fno-builtin)
