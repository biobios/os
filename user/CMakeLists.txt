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
add_custom_command(TARGET init POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:init> ${CMAKE_SOURCE_DIR}/assets/init.elf
)
