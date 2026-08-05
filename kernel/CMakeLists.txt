add_subdirectory(dependencies/ozlib)

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
    --target=x86_64-unknown-none-elf
    -O2
    -Wall
    -Wextra
    -Wpadded
    -g
    -ffreestanding
    -mno-red-zone
    -mcmodel=kernel
    -fno-exceptions
    -fno-rtti
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
