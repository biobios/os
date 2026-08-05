ENTRY(init)

KERNEL_PHYS_BASE = 0x00100000;
KERNEL_VIRT_BASE = 0xFFFFFFFF80100000;
OFFSET = KERNEL_VIRT_BASE - KERNEL_PHYS_BASE;

SECTIONS
{
    . = KERNEL_VIRT_BASE;

    __kernel_virt_start = .;
    __kernel_phys_start = . - OFFSET;

    .boot ALIGN(4K) : AT(ADDR(.boot) - OFFSET)
    {
        *(.boot .boot.*)
    }

    .text ALIGN(4K) : AT(ADDR(.text) - OFFSET)
    {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - OFFSET)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - OFFSET)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - OFFSET)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_virt_end = .;
    __kernel_phys_end = . - OFFSET;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
