# Interpreter benchmark: a mix of loads, stores, ALU, compare and branch.
	.section .text
	.org 0x100
	.globl _start
_start:
	lis	%r9, 0x10		# data at 0x100000
	lis	%r5, 2000000@h
	ori	%r5, %r5, 2000000@l
	mtctr	%r5
	li	%r3, 0
	li	%r4, 1
1:	lwz	%r6, 0(%r9)
	add	%r3, %r3, %r4
	rlwinm	%r7, %r3, 3, 0, 28
	xor	%r6, %r6, %r7
	stw	%r6, 4(%r9)
	addi	%r4, %r4, 3
	cmpw	%r3, %r4
	lbz	%r8, 7(%r9)
	subf	%r8, %r8, %r3
	stw	%r8, 8(%r9)
	bdnz	1b
2:	b	2b
