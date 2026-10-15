# Supervisor level tests of the 603 emulation: exceptions, the software
# loaded TLBs with a real miss handler (after the example handlers of the
# MPC603e User's Manual), BATs, protection, TGPR, rfi, the decrementer,
# doze, trace, string/multiple instructions and branches.
#
# Loaded at physical address 0 and started at 0x4000 in real mode. Every
# CHECK records { expected, actual, name } at r29; the program ends with
# 0xd0d0d0d0 at DONE and loops. tests/linux/cpu-unit-tests.cpp evaluates
# the records.

	.set LOG_VEC,	0x3f00
	.set LOG_SRR0,	0x3f04
	.set LOG_SRR1,	0x3f08
	.set LOG_DAR,	0x3f0c
	.set LOG_DSISR,	0x3f10
	.set LOG_EXTRA,	0x3f14
	.set COUNTS,	0x3e00		# one word per vector (vector >> 8)
	.set DONE,	0x3ffc
	.set CHECKS,	0x6000		# check records
	.set HTAB,	0x00010000	# 64 KB hashed page table
	.set VSID,	0x123
	.set SR2,	0x20000000 | VSID	# Kp = 1, Ks = 0

	.set SPRG0, 272
	.set SPRG1, 273
	.set SPRG2, 274
	.set SPRG3, 275
	.set DMISS, 976
	.set DCMP, 977
	.set HASH1, 978
	.set HASH2, 979
	.set IMISS, 980
	.set ICMP, 981
	.set RPA, 982
	.set HID0, 1008
	.set DEC, 22

	.macro CHECK reg, value, name
	lis	%r30, (\value)@h
	ori	%r30, %r30, (\value)@l
	stw	%r30, 0(%r29)
	stw	\reg, 4(%r29)
	lis	%r30, 98f@ha
	addi	%r30, %r30, 98f@l
	stw	%r30, 8(%r29)
	addi	%r29, %r29, 12
	.pushsection .rodata
98:	.string "\name"
	.popsection
	.endm

	.macro LOAD reg, value
	lis	\reg, (\value)@h
	ori	\reg, \reg, (\value)@l
	.endm

	# Generic handler: save r3/r4, log, count, then continue at 'next'.
	.macro VECTOR vec, next
	.org \vec
	mtspr	SPRG0, %r3
	mtspr	SPRG1, %r4
	li	%r3, \vec
	b	\next
	.endm

	.section .text
	.globl _vectors
_vectors:
	VECTOR 0x100, log_skip
	VECTOR 0x200, log_skip
	VECTOR 0x300, log_skip
	VECTOR 0x400, log_return_lr
	VECTOR 0x500, log_retry
	VECTOR 0x600, log_skip
	VECTOR 0x700, log_skip
	VECTOR 0x800, log_enable_fp
	VECTOR 0x900, log_decrementer
	VECTOR 0xc00, log_syscall
	VECTOR 0xd00, log_trace

	# ----------------------------------------------- instruction TLB miss
	.org 0x1000
itlb_miss:
	mfspr	%r2, HASH1
	mfctr	%r0
	mfspr	%r3, ICMP
im0:	li	%r1, 8
	mtctr	%r1
	subi	%r2, %r2, 8
im1:	lwzu	%r1, 8(%r2)
	cmpw	%r1, %r3
	bdnzf	eq, im1
	bne	im_secondary
	lwz	%r1, 4(%r2)
	ori	%r1, %r1, 0x100		# referenced
	stw	%r1, 4(%r2)
	mtctr	%r0
	mfspr	%r0, IMISS
	mtspr	RPA, %r1
	mfsrr1	%r3
	tlbli	%r0
	mtcrf	0x80, %r3
	lwz	%r1, COUNTS + 0x40(0)	# count ITLB reloads
	addi	%r1, %r1, 1
	stw	%r1, COUNTS + 0x40(0)
	rfi
im_secondary:
	andi.	%r1, %r3, 0x40
	bne	im_isi
	mfspr	%r2, HASH2
	ori	%r3, %r3, 0x40
	b	im0
im_isi:
	mfsrr1	%r3
	andi.	%r2, %r3, 0xffff
	addis	%r2, %r2, 0x4000	# SRR1[1]: page not found
	mtctr	%r0
	mtcrf	0x80, %r3
	mtsrr1	%r2
	mfmsr	%r0
	xoris	%r0, %r0, 0x2		# clear TGPR
	mtmsr	%r0
	ba	0x400

	# ------------------------------------------------ data TLB miss on load
	.org 0x1100
dtlb_load_miss:
	mfspr	%r2, HASH1
	mfctr	%r0
	mfspr	%r3, DCMP
dm0:	li	%r1, 8
	mtctr	%r1
	subi	%r2, %r2, 8
dm1:	lwzu	%r1, 8(%r2)
	cmpw	%r1, %r3
	bdnzf	eq, dm1
	bne	dm_secondary
	lwz	%r1, 4(%r2)
	ori	%r1, %r1, 0x100
	stw	%r1, 4(%r2)
	mtctr	%r0
	mfspr	%r0, DMISS
	mtspr	RPA, %r1
	mfsrr1	%r3
	tlbld	%r0
	mtcrf	0x80, %r3
	lwz	%r1, COUNTS + 0x44(0)	# count DTLB load reloads
	addi	%r1, %r1, 1
	stw	%r1, COUNTS + 0x44(0)
	rfi
dm_secondary:
	andi.	%r1, %r3, 0x40
	bne	dm_dsi
	mfspr	%r2, HASH2
	ori	%r3, %r3, 0x40
	b	dm0
dm_dsi:
	mfsrr1	%r3
	rlwinm	%r1, %r3, 9, 6, 6	# SRR1[15] (store) -> DSISR[6]
	addis	%r1, %r1, 0x4000	# DSISR[1]: not found
	b	synth_dsi

	# ----------------------------------------------- data TLB miss on store
	.org 0x1200
dtlb_store_miss:
	mfspr	%r2, HASH1
	mfctr	%r0
	mfspr	%r3, DCMP
sm0:	li	%r1, 8
	mtctr	%r1
	subi	%r2, %r2, 8
sm1:	lwzu	%r1, 8(%r2)
	cmpw	%r1, %r3
	bdnzf	eq, sm1
	bne	sm_secondary
	lwz	%r1, 4(%r2)
	# writable if PP = 2, or PP < 2 and the key (SRR1[12]) is 0
	andi.	%r3, %r1, 3
	cmpwi	%r3, 2
	beq	sm_ok
	cmpwi	%r3, 3
	beq	sm_protect
	mfsrr1	%r3
	andis.	%r3, %r3, 0x0008
	bne	sm_protect
sm_ok:
	ori	%r1, %r1, 0x180		# referenced and changed
	stw	%r1, 4(%r2)
	mtctr	%r0
	mfspr	%r0, DMISS
	mtspr	RPA, %r1
	mfsrr1	%r3
	tlbld	%r0
	mtcrf	0x80, %r3
	lwz	%r1, COUNTS + 0x48(0)	# count DTLB store reloads
	addi	%r1, %r1, 1
	stw	%r1, COUNTS + 0x48(0)
	rfi
sm_secondary:
	andi.	%r1, %r3, 0x40
	bne	dm_dsi
	mfspr	%r2, HASH2
	ori	%r3, %r3, 0x40
	b	sm0
sm_protect:
	mtctr	%r0
	lis	%r1, 0x0a00		# DSISR: protection | store
	b	synth_dsi

	.org 0x1300
	# (instruction address breakpoint, unused)
	mtspr	SPRG0, %r3
	mtspr	SPRG1, %r4
	li	%r3, 0x1300
	b	log_skip

	.org 0x1400
	mtspr	SPRG0, %r3
	mtspr	SPRG1, %r4
	li	%r3, 0x1400
	b	log_retry

	.org 0x1500
	# r1 = DSISR. Leave the TLB miss handler for the DSI handler.
synth_dsi:
	mtdsisr	%r1
	mfspr	%r1, DMISS
	mtdar	%r1
	mfsrr1	%r3
	mtcrf	0x80, %r3
	mfmsr	%r0
	xoris	%r0, %r0, 0x2		# clear TGPR
	mtmsr	%r0
	ba	0x300

	# ------------------------------------------------- handler tails
	# r3 = vector; r3 and r4 are saved in SPRG0/SPRG1.
log_common:
	stw	%r3, LOG_VEC(0)
	mfsrr0	%r4
	stw	%r4, LOG_SRR0(0)
	mfsrr1	%r4
	stw	%r4, LOG_SRR1(0)
	mfdar	%r4
	stw	%r4, LOG_DAR(0)
	mfdsisr	%r4
	stw	%r4, LOG_DSISR(0)
	srwi	%r3, %r3, 6
	lwz	%r4, COUNTS(%r3)
	addi	%r4, %r4, 1
	stw	%r4, COUNTS(%r3)
	blr

log_skip:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	mfsrr0	%r4
	addi	%r4, %r4, 4
	mtsrr0	%r4
	b	restore

log_retry:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	b	restore

log_return_lr:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	mflr	%r4
	mtsrr0	%r4
	b	restore

log_enable_fp:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	mfsrr1	%r4
	ori	%r4, %r4, 0x2000	# MSR[FP]
	mtsrr1	%r4
	b	restore

log_decrementer:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	LOAD	%r4, 0x7fffffff
	mtspr	DEC, %r4
	b	restore

log_syscall:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	mfspr	%r3, SPRG0
	cmpwi	%r3, 0x1234		# request: return in supervisor mode
	bne	restore
	mfsrr1	%r4
	rlwinm	%r4, %r4, 0, 18, 16	# clear MSR[PR]
	mtsrr1	%r4
	b	restore

log_trace:
	mtspr	SPRG2, %r5
	mflr	%r5
	bl	log_common
	mtlr	%r5
	mfspr	%r5, SPRG2
	lwz	%r4, COUNTS + (0xd00 >> 6)(0)
	cmpwi	%r4, 3
	blt	restore
	mfsrr1	%r4
	rlwinm	%r4, %r4, 0, 22, 20	# clear MSR[SE]
	mtsrr1	%r4

restore:
	mfspr	%r4, SPRG1
	mfspr	%r3, SPRG0
	rfi

	# =============================================================== tests
	.org 0x4000
	.globl _start
_start:
	LOAD	%r29, CHECKS
	li	%r0, 0
	# clear the logs
	li	%r3, 0
	li	%r4, 0x3e00
1:	stw	%r3, 0(%r4)
	addi	%r4, %r4, 4
	cmpwi	%r4, 0x4000
	blt	1b

	# ---------------------------------------------------- system call
	li	%r3, 0x55
sc_site:
	sc
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0xc00, "sc vector"
	lwz	%r4, LOG_SRR0(0)
	LOAD	%r5, sc_site + 4
	CHECK	%r4, sc_site + 4, "sc srr0"
	CHECK	%r3, 0x55, "sc preserves r3"

	# ---------------------------------------------------- illegal
ill_site:
	.long	0			# illegal instruction
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0x700, "illegal vector"
	lwz	%r4, LOG_SRR1(0)
	rlwinm	%r4, %r4, 0, 0, 15
	CHECK	%r4, 0x00080000, "illegal srr1 reason"
	lwz	%r4, LOG_SRR0(0)
	CHECK	%r4, ill_site, "illegal srr0"
	# fsqrt is not implemented by the 603
	mfmsr	%r4
	ori	%r4, %r4, 0x2000
	mtmsr	%r4
	isync
	fsqrt	%f1, %f2
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0x700, "fsqrt illegal"
	# tlbia neither
	tlbia
	lwz	%r4, LOG_SRR1(0)
	rlwinm	%r4, %r4, 0, 0, 15
	CHECK	%r4, 0x00080000, "tlbia illegal"

	# ---------------------------------------------------- trap
	li	%r6, 5
	twi	4, %r6, 5		# trap if equal
	lwz	%r4, LOG_SRR1(0)
	rlwinm	%r4, %r4, 0, 0, 15
	CHECK	%r4, 0x00020000, "trap reason"
	li	%r4, 0
	stw	%r4, LOG_VEC(0)
	twi	8, %r6, 5		# trap if greater: no trap
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0, "no trap"

	# ---------------------------------------------------- user mode
	mfmsr	%r4
	ori	%r4, %r4, 0x4000	# MSR[PR]
	LOAD	%r5, user_code
	mtsrr0	%r5
	mtsrr1	%r4
	rfi
user_code:
	mfmsr	%r6			# privileged
	li	%r3, 0x1234
	sc				# back to supervisor
	lwz	%r4, COUNTS + (0x700 >> 6)(0)
	CHECK	%r4, 5, "privileged count"
	lwz	%r4, LOG_SRR1(0)	# srr1 of the sc: PR was set
	andi.	%r4, %r4, 0x4000
	CHECK	%r4, 0x4000, "sc from user srr1 PR"
	mfmsr	%r4
	andi.	%r4, %r4, 0x4000
	CHECK	%r4, 0, "back in supervisor"

	# ---------------------------------------------------- alignment
	LOAD	%r5, 0x7002
	lwarx	%r6, 0, %r5
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0x600, "lwarx alignment vector"
	lwz	%r4, LOG_DAR(0)
	CHECK	%r4, 0x7006, "lwarx alignment dar (ea + 4)"
	lwz	%r4, LOG_DSISR(0)
	CHECK	%r4, 0x000000c0, "lwarx alignment dsisr"
	lfd	%f1, 1(%r5)		# FP access not word aligned
	lwz	%r4, LOG_DAR(0)
	CHECK	%r4, 0x7003, "lfd alignment dar"
	lwz	%r4, LOG_DSISR(0)
	CHECK	%r4, 0x00002425, "lfd alignment dsisr"
	lwz	%r6, 1(%r5)		# integer misaligned: no exception
	lwz	%r4, COUNTS + (0x600 >> 6)(0)
	CHECK	%r4, 2, "misaligned lwz ok"

	# ---------------------------------------------------- FP unavailable
	mfmsr	%r4
	rlwinm	%r4, %r4, 0, 19, 17	# clear MSR[FP]
	mtmsr	%r4
	isync
	fmr	%f3, %f4
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0x800, "fp unavailable"
	mfmsr	%r4
	andi.	%r4, %r4, 0x2000
	CHECK	%r4, 0x2000, "fp enabled after handler"

	# ---------------------------------------------------- multiple/string
	LOAD	%r5, 0x7100
	LOAD	%r20, 0x11111111
	LOAD	%r21, 0x22222222
	LOAD	%r28, 0x88888888
	stmw	%r20, 0(%r5)
	li	%r20, 0
	li	%r21, 0
	lmw	%r20, 0(%r5)
	CHECK	%r21, 0x22222222, "lmw r21"
	lwz	%r4, 32(%r5)
	CHECK	%r4, 0x88888888, "stmw r28"
	li	%r4, 3
	lmw	%r30, 1(%r5)		# misaligned lmw: alignment
	lwz	%r4, LOG_DAR(0)
	CHECK	%r4, 0x7105, "lmw alignment dar"

	# ---------------------------------------------------- branches
	li	%r6, 3
	mtctr	%r6
	li	%r7, 0
2:	addi	%r7, %r7, 1
	bdnz	2b
	CHECK	%r7, 3, "bdnz loop"
	mfctr	%r4
	CHECK	%r4, 0, "bdnz ctr"
	li	%r6, 2
	mtctr	%r6
	cmpwi	%r6, 2			# eq
	bdnzt	eq, 3f			# ctr 1, eq: taken
	b	4f
3:	li	%r7, 0x77
4:	CHECK	%r7, 0x77, "bdnzt taken"
	bdzf	eq, 5f			# ctr 0, eq true: not taken (cond)
	li	%r7, 0x78
5:	CHECK	%r7, 0x78, "bdzf not taken"
	bl	6f
6:	mflr	%r4
	LOAD	%r5, 6b
	CHECK	%r4, 6b, "bl link"
	LOAD	%r5, 7f
	mtlr	%r5
	li	%r7, 1
	blrl				# lr = next, branch to 7f
	li	%r7, 2
7:	CHECK	%r7, 1, "blrl target"
	LOAD	%r5, 8f
	mtctr	%r5
	bctrl
8:	mflr	%r4
	CHECK	%r4, 8b, "bctrl link"
	li	%r6, 1
	mtctr	%r6
	crset	4*cr1+gt
	bdnzt	4*cr1+gt, 9f		# ctr becomes 0: not taken
	li	%r7, 9
9:	CHECK	%r7, 9, "bdnzt ctr zero not taken"

	# ---------------------------------------------------- paging setup
	# Clear the hashed page table.
	LOAD	%r5, HTAB
	li	%r6, 0
	li	%r7, 16384
	mtctr	%r7
10:	stw	%r6, 0(%r5)
	addi	%r5, %r5, 4
	bdnz	10b

	.macro PTE ea, pa, pp, flags, secondary
	.set _h, ((VSID & 0x7ffff) ^ ((\ea >> 12) & 0xffff))
	.if \secondary
	.set _h, (~_h) & 0x7ffff
	.endif
	.set _pteg, HTAB | ((_h & 0x3ff) << 6)
	LOAD	%r5, _pteg
	LOAD	%r6, 0x80000000 | (VSID << 7) | (\secondary << 6) | ((\ea >> 22) & 0x3f)
	stw	%r6, 0(%r5)
	LOAD	%r6, \pa | \flags | \pp
	stw	%r6, 4(%r5)
	.endm

	PTE	0x20000000, 0x200000, 2, 0x180, 0	# RW, R and C set
	PTE	0x20001000, 0x201000, 2, 0x000, 0	# RW, C clear
	PTE	0x20002000, 0x202000, 3, 0x180, 0	# read only
	PTE	0x20003000, 0x203000, 2, 0x180, 1	# secondary hash
	PTE	0x20005000, 0x205000, 2, 0x180, 0	# code page
	PTE	0x20006000, 0x206000, 0, 0x180, 0	# key 1 (user): no access

	LOAD	%r5, HTAB
	mtsdr1	%r5
	LOAD	%r5, SR2
	mtsr	2, %r5
	# BATs: low 8 MB identity mapped for instructions and data,
	# EA 0x40000000 -> PA 0x300000 (128 KB) read/write.
	LOAD	%r5, 0x000000ff		# BEPI 0, BL 8 MB (0x3f << 2), Vs, Vp
	LOAD	%r6, 0x00000002		# BRPN 0, WIMG 0, PP 2
	mtspr	528, %r5		# IBAT0U
	mtspr	529, %r6		# IBAT0L
	mtspr	536, %r5		# DBAT0U
	mtspr	537, %r6		# DBAT0L
	LOAD	%r5, 0x40000003		# BEPI 0x40000000, BL 128 KB, Vs, Vp
	LOAD	%r6, 0x00300002
	mtspr	538, %r5		# DBAT1U
	mtspr	539, %r6		# DBAT1L
	# Invalidate the TLBs.
	li	%r5, 32
	mtctr	%r5
	li	%r6, 0
12:	tlbie	%r6
	addi	%r6, %r6, 0x1000
	bdnz	12b
	sync

	# Patterns in physical memory.
	LOAD	%r5, 0x200000
	LOAD	%r6, 0xcafe0000
	stw	%r6, 0x10(%r5)
	LOAD	%r5, 0x203000
	LOAD	%r6, 0xbeef0003
	stw	%r6, 0x20(%r5)
	LOAD	%r5, 0x300000
	LOAD	%r6, 0x5ba70000
	stw	%r6, 0x40(%r5)
	# A small routine on the code page: li r3, 0x77; blr
	LOAD	%r5, 0x205000
	LOAD	%r6, 0x38600077
	stw	%r6, 0(%r5)
	LOAD	%r6, 0x4e800020
	stw	%r6, 4(%r5)

	# Translation on.
	mfmsr	%r5
	ori	%r5, %r5, 0x30		# IR, DR
	mtmsr	%r5
	isync

	LOAD	%r5, 0x20000010
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0xcafe0000, "page load via TLB miss"
	lwz	%r4, COUNTS + 0x44(0)
	CHECK	%r4, 1, "one DTLB load reload"
	lwz	%r6, 4(%r5)		# TLB hit now
	lwz	%r4, COUNTS + 0x44(0)
	CHECK	%r4, 1, "no reload on hit"
	LOAD	%r5, 0x20003020
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0xbeef0003, "secondary hash page"

	# Store to a page with C = 0: store miss, handler sets C.
	LOAD	%r5, 0x20001008
	LOAD	%r6, 0x12345678
	stw	%r6, 0(%r5)
	lwz	%r4, COUNTS + 0x48(0)
	CHECK	%r4, 1, "store miss for C bit"
	lwz	%r7, 0(%r5)
	CHECK	%r7, 0x12345678, "store landed"
	# Load first, then store: the TLB has the entry with C = 0 now? No:
	# the store miss loaded it with C = 1. A second store must not miss.
	stw	%r6, 4(%r5)
	lwz	%r4, COUNTS + 0x48(0)
	CHECK	%r4, 1, "no second store miss"

	# Read only page: load works, store faults (TLB hit -> DSI).
	LOAD	%r5, 0x20002000
	lwz	%r6, 0(%r5)
	li	%r4, 0
	stw	%r4, LOG_VEC(0)
	stw	%r6, 0(%r5)
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0x300, "read only store DSI"
	lwz	%r4, LOG_DSISR(0)
	CHECK	%r4, 0x0a000000, "read only store dsisr"
	lwz	%r4, LOG_DAR(0)
	CHECK	%r4, 0x20002000, "read only store dar"

	# Unmapped page: miss handler synthesizes a DSI.
	LOAD	%r5, 0x20004000
	lwz	%r6, 0(%r5)
	lwz	%r4, LOG_DSISR(0)
	CHECK	%r4, 0x40000000, "unmapped load dsisr"
	lwz	%r4, LOG_DAR(0)
	CHECK	%r4, 0x20004000, "unmapped load dar"

	# BAT mapped data.
	LOAD	%r5, 0x40000040
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0x5ba70000, "BAT load"

	# Code on a mapped page (ITLB miss).
	LOAD	%r5, 0x20005000
	mtctr	%r5
	li	%r3, 0
	bctrl
	CHECK	%r3, 0x77, "code on mapped page"
	lwz	%r4, COUNTS + 0x40(0)
	CHECK	%r4, 1, "one ITLB reload"
	# Jump to an unmapped page: ISI (handler returns to LR).
	LOAD	%r5, 0x20007000
	mtctr	%r5
	bctrl
	lwz	%r4, LOG_VEC(0)
	CHECK	%r4, 0x400, "unmapped fetch ISI"
	lwz	%r4, LOG_SRR1(0)
	rlwinm	%r4, %r4, 0, 0, 15
	CHECK	%r4, 0x40000000, "ISI srr1 not found"

	# tlbie: remap 0x20000000 to another physical page.
	LOAD	%r5, 0x201000
	LOAD	%r6, 0xa5a5a5a5
	stw	%r6, 0x10(%r5)	# (via BAT? no: PA 0x201010 is in the BAT range)
	.set _h, ((VSID & 0x7ffff) ^ ((0x20000000 >> 12) & 0xffff))
	LOAD	%r5, HTAB | ((_h & 0x3ff) << 6)
	LOAD	%r6, 0x201000 | 0x180 | 2
	stw	%r6, 4(%r5)
	LOAD	%r5, 0x20000010
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0xcafe0000, "stale TLB before tlbie"
	tlbie	%r5
	sync
	lwz	%r6, 0(%r5)
	CHECK	%r6, 0xa5a5a5a5, "new mapping after tlbie"

	# TGPR: r0-r3 survive a TLB miss.
	li	%r0, 0x100
	li	%r1, 0x101
	li	%r2, 0x102
	li	%r3, 0x103
	LOAD	%r5, 0x20003024
	tlbie	%r5			# force a DTLB miss
	sync
	li	%r0, 0x100
	lwz	%r6, 0(%r5)
	CHECK	%r0, 0x100, "r0 after miss"
	CHECK	%r1, 0x101, "r1 after miss"
	CHECK	%r2, 0x102, "r2 after miss"
	CHECK	%r3, 0x103, "r3 after miss"
	mfmsr	%r4
	rlwinm	%r4, %r4, 0, 14, 14
	CHECK	%r4, 0, "TGPR clear after rfi"

	# User access to a key-protected page (PP = 0, Kp = 1): DSI.
	LOAD	%r5, 0x20006000
	lwz	%r6, 0(%r5)		# supervisor: allowed (key 0)
	mfmsr	%r4
	ori	%r4, %r4, 0x4000	# user mode
	LOAD	%r6, user_access
	mtsrr0	%r6
	mtsrr1	%r4
	rfi
user_access:
	lwz	%r6, 0(%r5)		# key 1, PP 0: protection fault
	li	%r3, 0x1234
	sc
	lwz	%r4, COUNTS + (0x300 >> 6)(0)
	CHECK	%r4, 3, "user DSI count"

	# Translation off again.
	mfmsr	%r5
	rlwinm	%r5, %r5, 0, 28, 25	# clear IR, DR
	mtmsr	%r5
	isync

	# ---------------------------------------------------- decrementer
	li	%r5, 200
	mtspr	DEC, %r5
	mfmsr	%r5
	ori	%r5, %r5, 0x8000	# EE
	mtmsr	%r5
	li	%r6, 0
13:	lwz	%r4, COUNTS + (0x900 >> 6)(0)
	cmpwi	%r4, 0
	bne	14f
	addi	%r6, %r6, 1
	cmpwi	%r6, 30000
	blt	13b
14:	CHECK	%r4, 1, "decrementer taken"
	mfmsr	%r5
	rlwinm	%r5, %r5, 0, 17, 15	# clear EE
	mtmsr	%r5

	# ---------------------------------------------------- doze
	li	%r5, 100
	mtspr	DEC, %r5
	mfspr	%r5, HID0
	oris	%r5, %r5, 0x0080	# DOZE
	mtspr	HID0, %r5
	mfmsr	%r5
	ori	%r5, %r5, 0x8000	# EE
	oris	%r5, %r5, 0x0004	# POW
	sync
	mtmsr	%r5
	isync
	lwz	%r4, COUNTS + (0x900 >> 6)(0)
	CHECK	%r4, 2, "woke from doze by decrementer"
	mfmsr	%r5
	rlwinm	%r5, %r5, 0, 17, 15	# clear EE
	rlwinm	%r5, %r5, 0, 14, 12	# clear POW
	mtmsr	%r5

	# ---------------------------------------------------- trace
	mfmsr	%r5
	ori	%r5, %r5, 0x400		# SE
	mtmsr	%r5
	nop
	nop
	nop
	nop
	nop
	lwz	%r4, COUNTS + (0xd00 >> 6)(0)
	CHECK	%r4, 3, "single step traces"

	# ---------------------------------------------------- done
	stw	%r29, 0x3ff8(0)
	LOAD	%r4, 0xd0d0d0d0
	stw	%r4, DONE(0)
15:	b	15b
