/* link.ld - psp-lite program layout (static PIE, linked at 0x80000000).
 *
 *   0x80000000  .text   (crt0's _start first)
 *               .rodata
 *               .data / .got
 *               .rela.dyn  (R_RISCV_RELATIVE entries applied by crt0)
 *               .bss
 *               .stack  (PSP_STACK_SIZE, default 64 KiB; sp starts at __stack_top)
 *   _end
 * Everything moves together when psp-lite loads the image at another base.
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

__psp_link_base = 0x80000000;
PROVIDE(__psp_stack_size = 0x10000);

SECTIONS
{
  . = __psp_link_base;
  .text : {
    KEEP(*(.text.init))
    *(.text .text.*)
  }
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  . = ALIGN(16);
  .data : { *(.data .data.* .sdata .sdata.*) }
  .got : { *(.got .got.*) }
  .rela.dyn : {
    __rela_start = .;
    *(.rela.dyn .rela.*)
    __rela_end = .;
  }
  .dynamic : { *(.dynamic) }
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .hash : { *(.hash) }
  . = ALIGN(16);
  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    . = ALIGN(4);
    __bss_end = .;
  }
  .stack (NOLOAD) : {
    . = ALIGN(16);
    . += __psp_stack_size;
    __stack_top = .;
  }
  _end = .;
  /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.gnu.hash) }
}
