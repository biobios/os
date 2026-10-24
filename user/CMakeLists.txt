cmake_minimum_required(VERSION 3.20)
project(OZ_USER_SPACE LANGUAGES C CXX ASM)

# Freestanding environment settings
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_FLAGS "-ffreestanding -fno-builtin -fno-stack-protector -nostdlib -mno-red-zone")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS} -fno-exceptions -fno-rtti")

include_directories(lib)

# Syscall library
add_library(oz_syscall STATIC lib/syscall.cpp)

# init process
option(ENABLE_BOOT_TESTS "Run self tests on boot" OFF)
set(INIT_SRCS src/init/main.cpp src/init/linker.ld)
if(ENABLE_BOOT_TESTS)
    list(APPEND INIT_SRCS src/init/tests.cpp)
    add_compile_definitions(ENABLE_BOOT_TESTS)
endif()

add_executable(init ${INIT_SRCS})
target_link_libraries(init oz_syscall)
target_link_options(init PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/init/linker.ld" "-nostdlib" "-static")

# Copy the built init.elf to assets so it will be included in the disk image


# console process
add_executable(console src/console/main.cpp src/console/linker.ld)
target_link_libraries(console oz_syscall)
target_link_options(console PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/console/linker.ld" "-nostdlib" "-static")




# vfs process
add_executable(vfs src/vfs/main.cpp src/vfs/VFS.cpp src/vfs/FAT32.cpp src/vfs/BufferCache.cpp src/vfs/linker.ld)
target_link_libraries(vfs oz_syscall ozstd)
target_include_directories(vfs PRIVATE ${CMAKE_SOURCE_DIR}/build/_deps/ozlib-src/include)
target_link_options(vfs PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/vfs/linker.ld" "-nostdlib" "-static")

# ahci process
add_executable(ahci src/ahci/main.cpp src/ahci/AHCI.cpp src/ahci/linker.ld)
target_link_libraries(ahci oz_syscall ozstd)
target_include_directories(ahci PRIVATE ${CMAKE_SOURCE_DIR}/build/_deps/ozlib-src/include)
target_link_options(ahci PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/ahci/linker.ld" "-nostdlib" "-static")

# xhci process (USB host controller + keyboard driver)
add_executable(xhci src/xhci/main.cpp src/xhci/xHCIDriver.cpp src/xhci/HIDKeyboard.cpp src/xhci/linker.ld)
target_link_libraries(xhci oz_syscall)
target_link_options(xhci PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/xhci/linker.ld" "-nostdlib" "-static")

# display process (Display server / GUI compositor)
add_executable(display src/display/main.cpp src/display/Font.cpp src/display/linker.ld)
target_link_libraries(display oz_syscall)
target_link_options(display PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/display/linker.ld" "-nostdlib" "-static")

# shell process (Interactive shell)
add_executable(shell src/shell/main.cpp src/display/Font.cpp src/shell/linker.ld)
target_link_libraries(shell oz_syscall)
target_link_options(shell PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/shell/linker.ld" "-nostdlib" "-static")

# manifest_test process (Verification of Step 2: Local Namespace & Manifest Sandboxing)
add_executable(manifest_test src/manifest_test/main.cpp src/manifest_test/linker.ld)
target_link_libraries(manifest_test oz_syscall)
target_link_options(manifest_test PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/manifest_test/linker.ld" "-nostdlib" "-static")

# cat process (Sandboxed CLI reader)
add_executable(cat src/cat/main.cpp src/cat/linker.ld)
target_link_libraries(cat oz_syscall)
target_link_options(cat PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/cat/linker.ld" "-nostdlib" "-static")

# head process (Sandboxed CLI head)
add_executable(head src/head/main.cpp src/head/linker.ld)
target_link_libraries(head oz_syscall)
target_link_options(head PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/head/linker.ld" "-nostdlib" "-static")

# memman process (Memory Manager)
add_executable(memman src/memman/main.cpp src/memman/linker.ld)
target_link_libraries(memman oz_syscall)
target_link_options(memman PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/memman/linker.ld" "-nostdlib" "-static")

# procman process (Process Manager)
add_executable(procman src/procman/main.cpp src/procman/linker.ld)
target_link_libraries(procman oz_syscall)
target_link_options(procman PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/procman/linker.ld" "-nostdlib" "-static")

# wm process (Tiling Window Manager)
add_executable(wm src/wm/main.cpp src/display/Font.cpp src/wm/linker.ld)
target_link_libraries(wm oz_syscall)
target_link_options(wm PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/wm/linker.ld" "-nostdlib" "-static")

# sched process (User-space Scheduler Daemon)
add_executable(sched src/sched/main.cpp src/sched/linker.ld)
target_link_libraries(sched oz_syscall)
target_link_options(sched PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/sched/linker.ld" "-nostdlib" "-static")

# =============================================================================
# Standalone Subsystem Test Runners
# =============================================================================
set(TEST_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tests/common/linker.ld")

# 1. ktest (Kernel Core Tests)
add_executable(ktest tests/ktest/main.cpp tests/common/linker.ld)
target_link_libraries(ktest oz_syscall)
target_link_options(ktest PRIVATE "-T${TEST_LINKER_SCRIPT}" "-nostdlib" "-static")

add_custom_target(initrd_ktest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_ktest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ktest> ${CMAKE_CURRENT_BINARY_DIR}/pack_ktest/init
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_ktest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_ktest init
    DEPENDS ktest
    COMMENT "Building test initrd for ktest"
    VERBATIM
)

# 2. stest (Scheduler & Radix-64 Notification Tests)
add_executable(stest tests/stest/main.cpp tests/common/linker.ld)
target_link_libraries(stest oz_syscall)
target_link_options(stest PRIVATE "-T${TEST_LINKER_SCRIPT}" "-nostdlib" "-static")

add_custom_target(initrd_stest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_stest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:stest> ${CMAKE_CURRENT_BINARY_DIR}/pack_stest/init
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sched> ${CMAKE_CURRENT_BINARY_DIR}/pack_stest/sched
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_stest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_stest init sched
    DEPENDS stest sched
    COMMENT "Building test initrd for stest"
    VERBATIM
)

# 3. vtest (VFS & Storage Tests)
add_executable(vtest tests/vtest/main.cpp tests/common/linker.ld)
target_link_libraries(vtest oz_syscall)
target_link_options(vtest PRIVATE "-T${TEST_LINKER_SCRIPT}" "-nostdlib" "-static")

add_custom_target(initrd_vtest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_vtest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:vtest> ${CMAKE_CURRENT_BINARY_DIR}/pack_vtest/init
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ahci> ${CMAKE_CURRENT_BINARY_DIR}/pack_vtest/ahci
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:vfs> ${CMAKE_CURRENT_BINARY_DIR}/pack_vtest/vfs
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_vtest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_vtest init ahci vfs
    DEPENDS vtest ahci vfs
    COMMENT "Building test initrd for vtest"
    VERBATIM
)

# 4. guitest (Display & WM GUI Tests)
add_executable(guitest tests/guitest/main.cpp tests/common/linker.ld)
target_link_libraries(guitest oz_syscall)
target_link_options(guitest PRIVATE "-T${TEST_LINKER_SCRIPT}" "-nostdlib" "-static")

add_custom_target(initrd_guitest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_guitest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:guitest> ${CMAKE_CURRENT_BINARY_DIR}/pack_guitest/init
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:display> ${CMAKE_CURRENT_BINARY_DIR}/pack_guitest/display
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:wm> ${CMAKE_CURRENT_BINARY_DIR}/pack_guitest/wm
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_guitest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_guitest init display wm
    DEPENDS guitest display wm
    COMMENT "Building test initrd for guitest"
    VERBATIM
)

# 5. xhcitest (xHCI Driver & Keyboard Tests)
add_executable(xhcitest tests/xhcitest/main.cpp tests/common/linker.ld)
target_link_libraries(xhcitest oz_syscall)
target_link_options(xhcitest PRIVATE "-T${TEST_LINKER_SCRIPT}" "-nostdlib" "-static")

add_custom_target(initrd_xhcitest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_xhcitest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:xhcitest> ${CMAKE_CURRENT_BINARY_DIR}/pack_xhcitest/init
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:xhci> ${CMAKE_CURRENT_BINARY_DIR}/pack_xhcitest/xhci
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_xhcitest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_xhcitest init xhci
    DEPENDS xhcitest xhci
    COMMENT "Building test initrd for xhcitest"
    VERBATIM
)

add_custom_target(initrd ALL
    COMMAND tar -cvf ${CMAKE_SOURCE_DIR}/assets/initrd.tar -C ${CMAKE_CURRENT_BINARY_DIR} init console vfs ahci xhci display shell manifest_test cat head memman procman wm sched
    COMMAND cp ${CMAKE_CURRENT_BINARY_DIR}/shell ${CMAKE_CURRENT_BINARY_DIR}/manifest_test ${CMAKE_CURRENT_BINARY_DIR}/cat ${CMAKE_CURRENT_BINARY_DIR}/head ${CMAKE_SOURCE_DIR}/assets/
    DEPENDS init console vfs ahci xhci display shell manifest_test cat head memman procman wm sched
)
target_sources(oz_syscall PRIVATE lib/malloc.cpp)


