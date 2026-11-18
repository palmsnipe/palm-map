ENTRY(MapNative)
SECTIONS {
    . = 0;
    .text : {
        KEEP(*(.entry))
        *(.text .text.* .rodata .rodata.*)
        . = ALIGN(4);
    }
    .forbidden : { *(.got .got.* .data .data.* .bss .bss.* COMMON) }
    ASSERT(SIZEOF(.forbidden) == 0, "ARMlet must be position independent")
    /DISCARD/ : { *(.comment .ARM.exidx* .ARM.extab* .note*) }
}
