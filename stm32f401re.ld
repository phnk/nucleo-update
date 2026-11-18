/* Linker script: where things go in the STM32F401RE's memory */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 16K   /* sector 0: main.cpp         */
                                                      /* sector 1 (0x08004000): the */
                                                      /* saved update, see main.cpp */
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K   /* globals + stack            */
    SPARE (rwx) : ORIGIN = 0x20010000, LENGTH = 32K   /* updates run here           */
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* stack starts at top of RAM, grows down */

SECTIONS
{
    /* Code and constants in flash, vector table first */
    .text : { KEEP(*(.isr_vector)) *(.text*) *(.rodata*) } > FLASH

    /* Initialised globals: live in RAM, initial values stored in flash */
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
    _sidata = LOADADDR(.data);

    /* Zero-initialised globals */
    .bss : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM

    /* Not touched by the startup code: where updates are copied to, and
       variables that must survive a reset */
    .update_slot (NOLOAD) : { *(.update_slot*) } > SPARE
    .noinit      (NOLOAD) : { *(.noinit*) } > SPARE
}
