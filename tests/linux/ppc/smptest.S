# Two CPUs increment a shared counter 100000 times each with lwarx/stwcx.,
# then count themselves as done. Both start at 0x100 with r3 = CPU index.
	.section .text
	.org 0x100
	.globl _start
_start:
	li	%r4, 0x1000		# counter; 0x1004 = done count
	lis	%r5, 100000@h
	ori	%r5, %r5, 100000@l
	mtctr	%r5
	li	%r10, 0			# successful stwcx. of this CPU
1:	lwarx	%r6, 0, %r4
	addi	%r6, %r6, 1
	stwcx.	%r6, 0, %r4
	bne-	1b
	addi	%r10, %r10, 1
	bdnz	1b
	slwi	%r8, %r3, 2
	addi	%r8, %r8, 0x1100
	stw	%r10, 0(%r8)		# 0x1100 + 4 * cpu
	addi	%r7, %r4, 4
2:	lwarx	%r6, 0, %r7
	addi	%r6, %r6, 1
	stwcx.	%r6, 0, %r7
	bne-	2b
3:	b	3b
