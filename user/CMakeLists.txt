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
if(DEFINED ozlib_SOURCE_DIR)
    include_directories(${ozlib_SOURCE_DIR}/include)
endif()

# Syscall & Slot Allocator library
add_library(oz_syscall STATIC
    lib/syscall.cpp
    lib/malloc.cpp
    lib/cspace_allocator.cpp
)

# Common font asset library
add_library(oz_font STATIC
    ${CMAKE_SOURCE_DIR}/common/src/Font.cpp
)

# Common user-space linker script and target helper
set(USER_COMMON_LD "${CMAKE_CURRENT_SOURCE_DIR}/common.ld")

function(oz_add_user_executable TARGET)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${TARGET} ${ARG_SOURCES} "${USER_COMMON_LD}")
    target_link_libraries(${TARGET} PRIVATE oz_syscall ${ARG_LIBS})
    target_link_options(${TARGET} PRIVATE "-T${USER_COMMON_LD}" "-nostdlib" "-static")
endfunction()

# =============================================================================
# User-space Daemons and Commands
# =============================================================================

# init process
oz_add_user_executable(init SOURCES src/init/main.cpp)

# console process
oz_add_user_executable(console SOURCES src/console/main.cpp)

# vfs process
oz_add_user_executable(vfs
    SOURCES src/vfs/main.cpp src/vfs/VFS.cpp src/vfs/FAT32.cpp src/vfs/BufferCache.cpp
    LIBS ozstd
)

# ahci process
oz_add_user_executable(ahci
    SOURCES src/ahci/main.cpp src/ahci/AHCI.cpp
    LIBS ozstd
)

# xhci process (USB host controller + keyboard driver)
oz_add_user_executable(xhci
    SOURCES src/xhci/main.cpp src/xhci/xHCIDriver.cpp src/xhci/HIDKeyboard.cpp
)

# display process (Display server / GUI compositor)
oz_add_user_executable(display
    SOURCES src/display/main.cpp
    LIBS oz_font
)

# shell process (Interactive shell)
oz_add_user_executable(shell
    SOURCES src/shell/main.cpp
    LIBS oz_font
)

# manifest_test process (Local Namespace & Manifest Sandboxing)
oz_add_user_executable(manifest_test SOURCES src/manifest_test/main.cpp)

# cat process (Sandboxed CLI reader)
oz_add_user_executable(cat SOURCES src/cat/main.cpp)

# head process (Sandboxed CLI head)
oz_add_user_executable(head SOURCES src/head/main.cpp)

# memman process (Memory Manager)
oz_add_user_executable(memman SOURCES src/memman/main.cpp)

# drivman process (Device Manager)
oz_add_user_executable(drivman SOURCES src/drivman/main.cpp)

# procman process (Process Manager)
oz_add_user_executable(procman SOURCES src/procman/main.cpp)

# wm process (Tiling Window Manager)
oz_add_user_executable(wm
    SOURCES src/wm/main.cpp
    LIBS oz_font
)

# sched process (User-space Scheduler Daemon)
oz_add_user_executable(sched SOURCES src/sched/main.cpp)

# =============================================================================
# Standalone Subsystem Test Runners
# =============================================================================

# 1. ktest (Kernel Core Tests)
oz_add_user_executable(ktest SOURCES tests/ktest/main.cpp)
add_custom_target(initrd_ktest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_ktest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ktest> ${CMAKE_CURRENT_BINARY_DIR}/pack_ktest/init
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_ktest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_ktest init
    DEPENDS ktest
    COMMENT "Building test initrd for ktest"
    VERBATIM
)

# 2. stest (Scheduler & Radix-64 Notification Tests)
oz_add_user_executable(stest SOURCES tests/stest/main.cpp)
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
oz_add_user_executable(vtest SOURCES tests/vtest/main.cpp)
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
oz_add_user_executable(guitest SOURCES tests/guitest/main.cpp)
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
oz_add_user_executable(xhcitest SOURCES tests/xhcitest/main.cpp)
add_custom_target(initrd_xhcitest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_xhcitest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:xhcitest> ${CMAKE_CURRENT_BINARY_DIR}/pack_xhcitest/init
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:xhci> ${CMAKE_CURRENT_BINARY_DIR}/pack_xhcitest/xhci
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_xhcitest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_xhcitest init xhci
    DEPENDS xhcitest xhci
    COMMENT "Building test initrd for xhcitest"
    VERBATIM
)

# 6. sandboxtest (Manifest & Sandboxing Tests)
oz_add_user_executable(sandboxtest SOURCES tests/sandboxtest/main.cpp)
add_custom_target(initrd_sandboxtest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_sandboxtest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sandboxtest> ${CMAKE_CURRENT_BINARY_DIR}/pack_sandboxtest/init
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:manifest_test> ${CMAKE_CURRENT_BINARY_DIR}/pack_sandboxtest/manifest_test
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cat> ${CMAKE_CURRENT_BINARY_DIR}/pack_sandboxtest/cat
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_sandboxtest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_sandboxtest init manifest_test cat
    DEPENDS sandboxtest manifest_test cat
    COMMENT "Building test initrd for sandboxtest"
    VERBATIM
)

# 7. ptest (Process & Memory Manager Tests)
oz_add_user_executable(ptest SOURCES tests/ptest/main.cpp)
add_custom_target(initrd_ptest
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/pack_ptest
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ptest> ${CMAKE_CURRENT_BINARY_DIR}/pack_ptest/init
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:memman> ${CMAKE_CURRENT_BINARY_DIR}/pack_ptest/memman
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:procman> ${CMAKE_CURRENT_BINARY_DIR}/pack_ptest/procman
    COMMAND tar -cvf ${CMAKE_BINARY_DIR}/initrd_ptest.tar -C ${CMAKE_CURRENT_BINARY_DIR}/pack_ptest init memman procman
    DEPENDS ptest memman procman
    COMMENT "Building test initrd for ptest"
    VERBATIM
)

# =============================================================================
# Full Distribution InitRD Image
# =============================================================================
add_custom_target(initrd ALL
    COMMAND tar -cvf ${CMAKE_SOURCE_DIR}/assets/initrd.tar -C ${CMAKE_CURRENT_BINARY_DIR} init console vfs ahci xhci display shell manifest_test cat head memman drivman procman wm sched
    COMMAND cp ${CMAKE_CURRENT_BINARY_DIR}/shell ${CMAKE_CURRENT_BINARY_DIR}/manifest_test ${CMAKE_CURRENT_BINARY_DIR}/cat ${CMAKE_CURRENT_BINARY_DIR}/head ${CMAKE_SOURCE_DIR}/assets/
    DEPENDS init console vfs ahci xhci display shell manifest_test cat head memman drivman procman wm sched
)
