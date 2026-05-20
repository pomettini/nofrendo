ENTRY(eventHandlerShim)
GROUP(libgcc.a libc.a libm.a)

SECTIONS
{
    .text :
    {
        /* Hot emulation path: pin both functions at the start of .text, each
           on its own 32-byte cache-line boundary.  Together they are ~15.8 KB
           and just fit inside the Rev B 16 KB I-cache.
           Requires -ffunction-sections (already set by the Playdate SDK). */
        . = ALIGN(32);
        *(.text.nes6502_execute)
        . = ALIGN(32);
        *(.text.ppu_scanline)

        . = ALIGN(4);
        *(.text)
        *(.text.*)

        KEEP(*(.init))
        KEEP(*(.fini))

        /* .ctors */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)

        /* .dtors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        *(.rodata*)

        KEEP(*(.eh_frame*))
    }

    .data :
    {
        __etext = .;

        __data_start__ = .;
        *(vtable)
        *(.data*)

        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN (__fini_array_end = .);

        . = ALIGN(4);
        __data_end__ = .;
    }

    .bss :
    {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        *(COM)
        . = ALIGN(4);
        __bss_end__ = .;
    }

    /DISCARD/ :
    {
        *(.ARM.exidx)
    }
}
