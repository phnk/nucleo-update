/* Linker script for the app (update.cpp): code that runs from RAM.
   It's built for the exact address the loader copies it to (the start
   of the SPARE region in stm32f401re.ld), so pointers inside it are right. */
ENTRY(update_header)

MEMORY
{
    SLOT (rwx) : ORIGIN = 0x20010000, LENGTH = 16K
}

SECTIONS
{
    /* One block: header first, then code, constants and globals.
       Zero-initialised globals are stored as zeros, so they work too. */
    .update : {
        KEEP(*(.update_header))
        *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON)
        . = ALIGN(4);
    } > SLOT

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.init_array*) *(.fini_array*) }
}
