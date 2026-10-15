/*
 * BeBox emulator - PowerPC instruction handlers.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_PPC_OPS_H
#define BEBOX_PPC_OPS_H

#include "PpcCpu.h"

namespace bebox {

struct PpcOps {
	static PpcHandler sPrimary[64];
	static PpcHandler sOp19[1024];
	static PpcHandler sOp31[1024];
	static PpcHandler sOp59[1024];
	static PpcHandler sOp63[1024];

	static void InitTables();
	// Integer, branch, load/store and system instructions.
	static void InitIntegerOps();
	// Floating point instructions.
	static void InitFpuOps();

	static void Illegal(PpcCpu& cpu, u32 instruction);

	// Sets the host's rounding mode to FPSCR[RN].
	static void SetHostRounding(u32 fpscr);
	// Recomputes FPSCR[VX] and FPSCR[FEX] from the other bits.
	static void UpdateFpscrSummary(PpcCpu& cpu);
};

// Instruction fields.
#define PPC_RD(i)		(((i) >> 21) & 31)
#define PPC_RS(i)		(((i) >> 21) & 31)
#define PPC_RA(i)		(((i) >> 16) & 31)
#define PPC_RB(i)		(((i) >> 11) & 31)
#define PPC_RC(i)		(((i) >> 6) & 31)
#define PPC_SIMM(i)		((s32)(s16)((i) & 0xffff))
#define PPC_UIMM(i)		((i) & 0xffff)
#define PPC_RCBIT(i)	(((i) & 1) != 0)
#define PPC_OEBIT(i)	(((i) & 0x400) != 0)

inline void
PpcSetCrField(PpcCpu& cpu, int field, u32 value)
{
	int shift = 28 - 4 * field;
	cpu.fCr = (cpu.fCr & ~(0xfu << shift)) | ((value & 0xf) << shift);
}

inline void
PpcSetCr0(PpcCpu& cpu, u32 result)
{
	u32 field = (s32)result < 0 ? 8 : result != 0 ? 4 : 2;
	if (cpu.fXerSo)
		field |= 1;
	cpu.fCr = (cpu.fCr & 0x0fffffff) | (field << 28);
}

}	// namespace bebox

#endif	// BEBOX_PPC_OPS_H
