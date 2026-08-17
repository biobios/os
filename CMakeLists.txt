cmake_minimum_required(VERSION 3.20)

# Default to clang/clang++ if not specified
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(CLANG_C_COMPILER clang)
    if(CLANG_C_COMPILER)
        set(CMAKE_C_COMPILER "${CLANG_C_COMPILER}")
    endif()
endif()

if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(CLANG_CXX_COMPILER clang++)
    if(CLANG_CXX_COMPILER)
        set(CMAKE_CXX_COMPILER "${CLANG_CXX_COMPILER}")
    endif()
endif()

project(OZ_OS LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(QEMU_EXECUTABLE qemu-system-x86_64)

# Configuration options
set(OVMF_PATH "/usr/share/ovmf/OVMF.fd" CACHE FILEPATH "Path to OVMF bios file")
set(HDD_IMAGE "${CMAKE_BINARY_DIR}/hdd.img" CACHE FILEPATH "Path to output HDD image")
set(HDD_SIZE_MB 200 CACHE STRING "Disk image size in MB")
set(QEMU_MEMORY "4G" CACHE STRING "QEMU memory size")

# Subdirectories
add_subdirectory(bootloader)
add_subdirectory(kernel)

# Custom target: image (creates / updates FAT32 disk image)
add_custom_target(image
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_image.py"
        --image "${HDD_IMAGE}"
        --size-mb "${HDD_SIZE_MB}"
        --bootloader "$<TARGET_FILE:bootloader>"
        --kernel "$<TARGET_FILE:kernel>"
    DEPENDS bootloader kernel "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_image.py"
    COMMENT "Generating and populating FAT32 disk image (${HDD_IMAGE})"
    VERBATIM
)
add_custom_target(img DEPENDS image)

# Custom target: run QEMU
if(QEMU_EXECUTABLE)
    add_custom_target(run
        COMMAND ${QEMU_EXECUTABLE}
            -m ${QEMU_MEMORY}
            -M q35
            -device qemu-xhci,id=xhci
            -device usb-kbd,bus=xhci.0
            -bios "${OVMF_PATH}"
            -hda "${HDD_IMAGE}"
        DEPENDS image
        COMMENT "Running OZ OS in QEMU"
        VERBATIM
    )

    add_custom_target(run-debug
        COMMAND ${QEMU_EXECUTABLE}
            -s -S
            -m ${QEMU_MEMORY}
            -M q35
            -device qemu-xhci,id=xhci
            -device usb-kbd,bus=xhci.0
            -bios "${OVMF_PATH}"
            -hda "${HDD_IMAGE}"
        DEPENDS image
        COMMENT "Running OZ OS in QEMU (Waiting for GDB on :1234)"
        VERBATIM
    )
endif()
