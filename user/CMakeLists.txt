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
add_executable(init src/init/main.cpp src/init/linker.ld)
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
add_executable(shell src/shell/main.cpp src/shell/linker.ld)
target_link_libraries(shell oz_syscall)
target_link_options(shell PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/shell/linker.ld" "-nostdlib" "-static")

# manifest_test process (Verification of Step 2: Local Namespace & Manifest Sandboxing)
add_executable(manifest_test src/manifest_test/main.cpp src/manifest_test/linker.ld)
target_link_libraries(manifest_test oz_syscall)
target_link_options(manifest_test PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/src/manifest_test/linker.ld" "-nostdlib" "-static")

add_custom_target(initrd ALL
    COMMAND tar -cvf ${CMAKE_SOURCE_DIR}/assets/initrd.tar -C ${CMAKE_CURRENT_BINARY_DIR} init console vfs ahci xhci display shell manifest_test
    DEPENDS init console vfs ahci xhci display shell manifest_test
)
target_sources(oz_syscall PRIVATE lib/malloc.cpp)

