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
option(ENABLE_BOOT_TESTS "Run self tests on boot" OFF)
set(OVMF_PATH "/usr/share/ovmf/OVMF.fd" CACHE FILEPATH "Path to OVMF bios file")
set(HDD_IMAGE "${CMAKE_BINARY_DIR}/hdd.img" CACHE FILEPATH "Path to output HDD image")
set(HDD_SIZE_MB 200 CACHE STRING "Disk image size in MB")
set(QEMU_MEMORY "4G" CACHE STRING "QEMU memory size")

# Subdirectories
add_subdirectory(bootloader)
add_subdirectory(kernel)
add_subdirectory(user)

set(HDD_KTEST_IMAGE "${CMAKE_BINARY_DIR}/hdd_ktest.img" CACHE FILEPATH "Path to kernel test HDD image")

# Custom target: image (creates / updates FAT32 disk image)
add_custom_target(image
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_image.py"
        --image "${HDD_IMAGE}"
        --size-mb "${HDD_SIZE_MB}"
        --bootloader "$<TARGET_FILE:bootloader>"
        --kernel "$<TARGET_FILE:kernel>"
        --assets "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    DEPENDS bootloader kernel initrd "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_image.py" "${CMAKE_CURRENT_SOURCE_DIR}/assets"
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
            -serial file:${CMAKE_CURRENT_SOURCE_DIR}/kernel.log
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
            -serial file:${CMAKE_CURRENT_SOURCE_DIR}/kernel.log
        DEPENDS image
        COMMENT "Running OZ OS in QEMU (Waiting for GDB on :1234)"
        VERBATIM
    )
endif()

# =============================================================================
# Subsystem Test Target Helper Function
# =============================================================================
function(add_os_test_target NAME INITRD_TARGET)
    set(TEST_IMG "${CMAKE_BINARY_DIR}/hdd_${NAME}.img")
    set(EXTRA_QEMU_ARGS ${ARGN})

    add_custom_target(image-${NAME}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/assets_${NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${CMAKE_BINARY_DIR}/assets_${NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/initrd_${NAME}.tar" "${CMAKE_BINARY_DIR}/assets_${NAME}/initrd.tar"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_image.py"
            --image "${TEST_IMG}"
            --size-mb "${HDD_SIZE_MB}"
            --bootloader "$<TARGET_FILE:bootloader>"
            --kernel "$<TARGET_FILE:kernel>"
            --assets "${CMAKE_BINARY_DIR}/assets_${NAME}"
        DEPENDS bootloader kernel ${INITRD_TARGET} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create_image.py" "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        COMMENT "Generating disk image for ${NAME} (${TEST_IMG})"
        VERBATIM
    )

    if(QEMU_EXECUTABLE)
        add_custom_target(run-${NAME}
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_qemu_test.py"
                ${QEMU_EXECUTABLE}
                -m ${QEMU_MEMORY}
                -M q35
                -bios "${OVMF_PATH}"
                -hda "${TEST_IMG}"
                -display none
                -serial stdio
                -device isa-debug-exit,iobase=0xf4,iosize=0x04
                ${EXTRA_QEMU_ARGS}
            DEPENDS image-${NAME}
            COMMENT "Running ${NAME} in QEMU"
            VERBATIM
        )
    endif()
endfunction()

add_os_test_target(ktest    initrd_ktest)
add_os_test_target(stest    initrd_stest)
add_os_test_target(vtest    initrd_vtest)
add_os_test_target(guitest  initrd_guitest)
add_os_test_target(xhcitest initrd_xhcitest -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0)

if(QEMU_EXECUTABLE)
    add_custom_target(test-all
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target run-ktest
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target run-stest
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target run-vtest
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target run-guitest
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target run-xhcitest
        COMMENT "Running all subsystem test suites"
        VERBATIM
    )
endif()
