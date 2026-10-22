/* MicroBlaze start-up: exception vectors at 0x0 and the reset entry point.
 *
 * Each vector is 8 bytes (imm + brai). They sit in their own sections so the
 * linker (which relaxes branches) can't move them; linker.ld places each one.
 */

	.section .vectors.reset, "ax"
	.globl	_vector_reset
_vector_reset:
	brai	_start

	.section .vectors.sw_exception, "ax"
_vector_sw_exception:
	brai	_exception_trap

	.section .vectors.interrupt, "ax"
_vector_interrupt:
	brai	irq_handler

	.section .vectors.debug_sw_break, "ax"
_vector_break:
	brai	_exception_trap

	.section .vectors.hw_exception, "ax"
_vector_hw_exception:
	brai	_exception_trap

	.text
	.globl	_start
	.ent	_start
_start:
	addik	r1, r0, _stack_top - 16     /* stack (grows down, in data_ram) */
	addik	r2, r0, _SDA2_BASE_         /* read-only small data anchor */
	addik	r13, r0, _SDA_BASE_         /* read-write small data anchor */
	brlid	r15, _cstart                /* copies .data, clears .bss, calls main */
	nop
1:	bri	1b
	.end	_start

/* Unexpected exception or break: park here; the host can see r17 via a dump later. */
	.globl	_exception_trap
	.weak	_exception_trap
	.ent	_exception_trap
_exception_trap:
	bri	_exception_trap
	.end	_exception_trap
