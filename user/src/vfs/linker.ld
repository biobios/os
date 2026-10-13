ENTRY(_start)

SECTIONS {
    . = 0x400000; /* Load address, common for x86_64 executables */
    
    .text : {
        *(.text .text.*)
    }
    
    .rodata : {
        *(.rodata .rodata.*)
    }
    
    .data : {
        *(.data .data.*)
    }
    
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
}
