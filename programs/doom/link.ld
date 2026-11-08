ENTRY(_start)

MEMORY
{
    IMAGE (rwx) : ORIGIN = 0x90900000, LENGTH = 0x180000
}

SECTIONS
{
    .text : {
        __image_start = .;
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
        __text_end = .;
        . = ALIGN(4);
        KEEP(*(.init_array .init_array.*))
        . = ALIGN(4);
    } > IMAGE
    .ARM.exidx : { *(.ARM.exidx*) } > IMAGE
    .data : {
        KEEP(*(.guard.data_lo))
        *(.data .data.*)
        KEEP(*(.guard.data_hi))
        . = ALIGN(4);
        __image_end = .;
    } > IMAGE
    .bss (NOLOAD) : {
        __bss_start = .;
        KEEP(*(.guard.bss_lo))
        *(.bss .bss.* COMMON)
        KEEP(*(.guard.bss_hi))
        . = ALIGN(4);
        __bss_end = .;
    } > IMAGE
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) }
}

__heap_start = 0x90a80000;
__heap_end = 0x91800000;
ASSERT(__bss_end <= __heap_start, "the image runs into the heap")
