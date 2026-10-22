/* MultiTX MicroBlaze firmware memory layout.
 *
 *   code_ram 0x0000_0000  256 KB  vectors, code, read-only data, .data load image
 *            0x0003_F000    4 KB  console mailbox (shared with the USB bridge)
 *   data_ram 0x0010_0000   64 KB  .data, .bss, heap, stack
 *
 * The CPU fetches code over the instruction LMB and reads constants from the
 * same BRAM through its AXI port; data_ram is LMB only (fast, CPU only).
 */

OUTPUT_ARCH(microblaze)
ENTRY(_vector_reset)

_STACK_SIZE = 16K;

MEMORY
{
	code    (rx)  : ORIGIN = 0x00000000, LENGTH = 0x3F000
	mailbox (rw)  : ORIGIN = 0x0003F000, LENGTH = 0x1000
	data    (rw)  : ORIGIN = 0x00100000, LENGTH = 64K
}

SECTIONS
{
	.vectors.reset          0x00 : { KEEP(*(.vectors.reset)) } > code
	.vectors.sw_exception   0x08 : { KEEP(*(.vectors.sw_exception)) } > code
	.vectors.interrupt      0x10 : { KEEP(*(.vectors.interrupt)) } > code
	.vectors.debug_sw_break 0x18 : { KEEP(*(.vectors.debug_sw_break)) } > code
	.vectors.hw_exception   0x20 : { KEEP(*(.vectors.hw_exception)) } > code

	.text 0x50 : {
		*(.text .text.*)
	} > code

	/* C runtime start-up (crti/crtbegin/crtend/crtn) and C++ constructors */
	.init : { KEEP(*(.init)) } > code
	PROVIDE(_init = __init);   /* newlib's __libc_init_array calls _init */
	.fini : { KEEP(*(.fini)) } > code
	.ctors : {
		KEEP(*crtbegin.o(.ctors))
		KEEP(*(EXCLUDE_FILE(*crtend.o) .ctors))
		KEEP(*(SORT(.ctors.*)))
		KEEP(*(.ctors))
	} > code
	.dtors : {
		KEEP(*crtbegin.o(.dtors))
		KEEP(*(EXCLUDE_FILE(*crtend.o) .dtors))
		KEEP(*(SORT(.dtors.*)))
		KEEP(*(.dtors))
	} > code
	.init_array : {
		PROVIDE_HIDDEN(__init_array_start = .);
		KEEP(*(SORT(.init_array.*)))
		KEEP(*(.init_array))
		PROVIDE_HIDDEN(__init_array_end = .);
	} > code

	.rodata : ALIGN(4) {
		*(.rodata .rodata.*)
	} > code

	/* C++ exception unwinding tables */
	.eh_frame : { KEEP(*(.eh_frame)) } > code
	.gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } > code

	.sdata2 : ALIGN(4) {
		_SDA2_BASE_ = . + 0x8000;
		*(.sdata2 .sdata2.*)
		*(.sbss2 .sbss2.*)
	} > code

	.data : ALIGN(4) {
		_data_start = .;
		*(.data .data.*)
		_SDA_BASE_ = . + 0x8000;
		*(.sdata .sdata.*)
		. = ALIGN(4);
		_data_end = .;
	} > data AT > code
	_data_load = LOADADDR(.data);

	.bss (NOLOAD) : ALIGN(4) {
		_bss_start = .;
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(8);
		_bss_end = .;
	} > data

	.heap (NOLOAD) : ALIGN(8) {
		_heap_start = .;
		. = ORIGIN(data) + LENGTH(data) - _STACK_SIZE;
		_heap_end = .;
	} > data

	.stack (NOLOAD) : ALIGN(8) {
		. += _STACK_SIZE;
		_stack_top = .;
	} > data

	.mailbox (NOLOAD) : {
		KEEP(*(.mailbox))
	} > mailbox

	_code_end = LOADADDR(.data) + SIZEOF(.data);

	/DISCARD/ : { *(.note.*) *(.comment) }
}
