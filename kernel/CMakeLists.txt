# Inject OS kernel cross-compilation flags to all subdirectories including ozlib
add_compile_options(
    --target=x86_64-unknown-none-elf
    -mcmodel=kernel
    -ffreestanding
    -mno-red-zone
)

include(FetchContent)
FetchContent_Declare(
    ozlib
    GIT_REPOSITORY https://github.com/biobios/ozlib.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(ozlib)

file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.hpp"
    "src/*.h"
)

add_executable(kernel
    ${KERNEL_SOURCES}
)

set_target_properties(kernel PROPERTIES
    OUTPUT_NAME "kernel"
    SUFFIX ".bin"
)

target_include_directories(kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(kernel PRIVATE
    -O2
    -Wall
    -Wextra
    -Wpadded
    -g
    -fno-exceptions
    -fno-rtti
    -fno-use-cxa-atexit
)

target_link_libraries(kernel PRIVATE
    ozstd
    commonstd
)

target_link_options(kernel PRIVATE
    --target=x86_64-unknown-none-elf
    -static
    -nostdlib
    -fuse-ld=lld
    -Wl,-z,norelro
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -Wl,-no-pie
)
