# Validation-only drivers. These are never part of a normal system image, they
# exist purely to exercise hardware interrupt delivery paths under QEMU. They
# are built with the add_module_target macro provided by modules/CMakeLists.txt,
# so this directory must be added after the modules directory.

if (VALI_EDU_SELFTEST)
    add_subdirectory(edu)
endif ()

if (VALI_ROCKER_SELFTEST)
    add_subdirectory(rocker)
endif ()
