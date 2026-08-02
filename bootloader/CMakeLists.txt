file(GLOB BOOTLOADER_SOURCES CONFIGURE_DEPENDS "*.c" "*.h")

add_executable(bootloader
    ${BOOTLOADER_SOURCES}
)

set_target_properties(bootloader PROPERTIES
    OUTPUT_NAME "BOOTX64"
    SUFFIX ".EFI"
)

target_include_directories(bootloader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(bootloader PRIVATE
    --target=x86_64-pc-win32-coff
    -Wall
    -Wextra
    -mno-red-zone
    -fno-stack-protector
    -fshort-wchar
)

target_link_options(bootloader PRIVATE
    --target=x86_64-pc-win32-coff
    -nostdlib
    -fuse-ld=lld-link
    -Wl,-subsystem:efi_application
    -Wl,-entry:uefi_main
)
