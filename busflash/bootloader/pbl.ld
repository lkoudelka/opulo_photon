/* PBL: last 1 KB page of STM32F031x6 flash, RAM limited to the first 4 KB. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08007C00, LENGTH = 1K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 3K   /* top 1 KB = stack */
}

SECTIONS
{
    .text : {
        KEEP(*(.pbl_header))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .data : { *(.data .data.*) } > RAM AT > FLASH
    ASSERT(SIZEOF(.data) == 0, "PBL must not use initialised data")

    .bss (NOLOAD) : { *(.bss .bss.* COMMON) } > RAM

    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
