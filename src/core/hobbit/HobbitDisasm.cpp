/*
 * BeBox emulator - AT&T ATT92010 "Hobbit" disassembler (traces, debugging).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The notation of the Programmer's Reference Manual: "$n" an immediate,
 * "*$a" an absolute operand, "Rn" a word at CSP + n, "*Rn" the word that
 * one points to, ":B"/":UB"/":H"/":UH" the operand size, "%REG" a control
 * register (CPU prefix).
 */
#include "HobbitDisasm.h"

#include <cstdarg>
#include <cstdio>

namespace bebox {

static const char* const kRegisters[16] = { "%r0", "%MSP", "%ISP", "%SP",
	"%CONFIG", "%PSW", "%SHAD", "%VB", "%STB", "%FAULT", "%ID", "%TIMER1",
	"%TIMER2", "%r13", "%r14", "%r15" };
static const char* const kSuffixes[4] = { ":B", ":UB", ":H", ":UH" };


static std::string
Format(const char* format, ...) __attribute__((format(printf, 1, 2)));

static std::string
Format(const char* format, ...)
{
	char buffer[128];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}


static s32
SignExtend(u32 value, int bits)
{
	u32 sign = 1u << (bits - 1);
	return (s32)((value ^ sign) - sign);
}


static std::string
Hex(s32 value)
{
	if (value < 0)
		return Format("-0x%x", (u32)-(s64)value);
	return Format("0x%x", (u32)value);
}


static std::string
GeneralOperand(int mode, u32 field, bool wide, bool cpu)
{
	s32 offset = wide ? (s32)field : SignExtend(field & 0xffff, 16);
	u32 address = wide ? field : field & 0xffff;
	if (cpu && mode == 7)
		return kRegisters[field & 15];
	if (mode < 4)
		return Format("*$0x%x%s", address, kSuffixes[mode]);
	if (mode < 8)
		return Format("R%d%s", offset, kSuffixes[mode - 4]);
	if (mode < 12)
		return Format("*R%d%s", offset, kSuffixes[mode - 8]);
	switch (mode) {
		case 0xc:
			return Format("*$0x%x", address);
		case 0xd:
			return Format("R%d", offset);
		case 0xe:
			return Format("*R%d", offset);
	}
	return "$" + Hex(offset);
}


static std::string
FlowOperand(int mode, u32 field, u32 pc)
{
	switch (mode) {
		case 0xc:
			return Format("**$0x%x", field);
		case 0xd:
			return Format("*R%d", (s32)field);
		case 0xe:
			return Format("0x%x", pc + field);
		case 0xf:
			return Format("*$0x%x", field);
	}
	return Format("<mode %x>", mode);
}


static const char*
DyadicName(int opcode)
{
	switch (opcode) {
		case 0x01: return "ORI";
		case 0x02: return "ANDI";
		case 0x03: return "ADDI";
		case 0x04: return "MOVA";
		case 0x05: return "UREM";
		case 0x06: return "MOV";
		case 0x07: return "DQM";
		case 0x0c: return "TADD";
		case 0x0d: return "TSUB";
		case 0x1d: return "CMPGT";
		case 0x1e: return "CMPHI";
		case 0x1f: return "CMPEQ";
		case 0x20: return "SUB";
		case 0x21: return "OR";
		case 0x22: return "AND";
		case 0x23: return "ADD";
		case 0x24: return "XOR";
		case 0x25: return "REM";
		case 0x26: return "MUL";
		case 0x27: return "DIV";
		case 0x2c: return "SHR";
		case 0x2d: return "USHR";
		case 0x2e: return "SHL";
		case 0x2f: return "UDIV";
		case 0x30: return "SUB3";
		case 0x31: return "OR3";
		case 0x32: return "AND3";
		case 0x33: return "ADD3";
		case 0x34: return "XOR3";
		case 0x35: return "REM3";
		case 0x36: return "MUL3";
		case 0x37: return "DIV3";
		case 0x3c: return "SHR3";
		case 0x3d: return "USHR3";
		case 0x3e: return "SHL3";
	}
	return nullptr;
}


static std::string
Disassemble(u32 pc, const std::function<u16(u32)>& fetch, int& length,
	bool cpu)
{
	u16 parcel = fetch(pc);
	if ((parcel & 0x8000) == 0) {
		length = 2;
		int opcode = (parcel >> 10) & 0x1f;
		u32 s = (parcel >> 5) & 0x1f;
		u32 d = parcel & 0x1f;
		u32 relative = pc + SignExtend(parcel & 0x3ff, 10) * 2;
		static const char* const kJumps[5] = { "JMP", "JMPFN", "JMPFY",
			"JMPTN", "JMPTY" };
		switch (opcode) {
			case 0x00:
				return Format("KCALL $0x%x", (parcel & 0x3ff) * 4);
			case 0x01:
				return Format("CALL 0x%x", relative);
			case 0x02:
			{
				u32 n = (parcel >> 2) & 0xff;
				switch (parcel & 3) {
					case 0:
						return Format("ENTER R%d",
							(s32)((0xffffff00 | n) * 16));
					case 1:
						return Format("CATCH R%u", n * 16);
					case 2:
						return Format("RETURN R%u", n * 16);
					default:
						return Format("POPN R%u", n * 16);
				}
			}
			case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
				return Format("%s 0x%x", kJumps[opcode - 3], relative);
			case 0x0a:
				return Format("MOV $%u,R%u", s * 4, d * 4);
			case 0x0b:
			{
				u32 subcode = parcel & 0x3ff;
				if (subcode >= 0x200)
					return Format("TRAP $0x%x", subcode);
				static const char* const kNiladics[11] = { "CPU", "KRET",
					"NOP", "FLUSHI", "FLUSHP", "CRET", "FLUSHD", nullptr,
					"TESTV", "TESTC", "CLRE" };
				if (subcode < 11 && kNiladics[subcode] != nullptr) {
					if (subcode == 0) {
						int next;
						std::string prefixed = Disassemble(pc + 2, fetch, next,
							true);
						length = 2 + next;
						return "CPU " + prefixed;
					}
					return kNiladics[subcode];
				}
				return Format(".parcel 0x%04x", parcel);
			}
			case 0x0d:
				return Format("ADD3 $%u,R%u", s * 4, d * 4);
			case 0x0e:
				return Format("AND3 $%d,R%u", SignExtend(s, 5), d * 4);
			case 0x0f:
				return Format("AND R%u,R%u", s * 4, d * 4);
			case 0x10:
				return Format("CMPEQ $%d,R%u", SignExtend(s, 5), d * 4);
			case 0x11:
				return Format("CMPGT R%u,R%u", s * 4, d * 4);
			case 0x12:
				return Format("CMPGT $%d,R%u", SignExtend(s, 5), d * 4);
			case 0x13:
				return Format("CMPEQ R%u,R%u", s * 4, d * 4);
			case 0x14:
				return Format("ADD $%d,R%u", SignExtend(s, 5), d * 4);
			case 0x15:
				return Format("ADD3 $%d,R%u", SignExtend(s, 5), d * 4);
			case 0x16:
				return Format("ADD R%u,R%u", s * 4, d * 4);
			case 0x17:
				return Format("ADD3 R%u,R%u", s * 4, d * 4);
			case 0x18:
				return Format("MOV R%u,R%u", s * 4, d * 4);
			case 0x19:
				return Format("MOV *R%u,R%u", s * 4, d * 4);
			case 0x1a:
				return Format("MOV R%u,*R%u", s * 4, d * 4);
			case 0x1b:
				return Format("MOV *R%u,*R%u", s * 4, d * 4);
			case 0x1c:
				return Format("MOV $%d,R%u", SignExtend(s, 5), d * 4);
			case 0x1d:
				return Format("MOVA R%u,R%u", s * 4, d * 4);
			case 0x1e:
				return Format("SHL3 $%u,R%u", s, d * 4);
			case 0x1f:
				return Format("SHR3 $%u,R%u", s, d * 4);
		}
		return Format(".parcel 0x%04x", parcel);
	}

	bool wide = (parcel & 0x4000) != 0;
	length = wide ? 10 : 6;
	u32 a = fetch(pc + 2);
	u32 b = fetch(pc + 4);
	u32 srcField = a;
	u32 dstField = b;
	if (wide) {
		srcField = (a << 16) | b;
		dstField = ((u32)fetch(pc + 6) << 16) | fetch(pc + 8);
	}
	int opcode = (parcel >> 8) & 0x3f;
	int srcMode = (parcel >> 4) & 15;
	int dstMode = parcel & 15;
	if (opcode == 0 && !wide) {
		u32 field = (a << 16) | b;
		static const char* const kMonadics[16] = { "KCALL", "CALL", "RETURN",
			"JMP", "JMPFN", "JMPFY", "JMPTN", "JMPTY", "CATCH", "ENTER",
			"LDRAA", "FLUSHPTE", "FLUSHPBE", "FLUSHDCE", nullptr, "POPN" };
		const char* name = kMonadics[dstMode];
		if (name == nullptr)
			return Format(".monadic 0x%x", dstMode);
		bool flow = dstMode == 1 || (dstMode >= 3 && dstMode <= 7)
			|| dstMode == 10;
		return std::string(name) + " " + (flow
			? FlowOperand(srcMode, field, pc)
			: GeneralOperand(srcMode, field, true, cpu));
	}
	const char* name = DyadicName(opcode);
	if (name == nullptr)
		return Format(".unimplemented 0x%02x", opcode);
	return std::string(name) + " " + GeneralOperand(srcMode, srcField, wide,
		cpu) + "," + GeneralOperand(dstMode, dstField, wide, cpu);
}


std::string
HobbitDisassemble(u32 pc, const std::function<u16(u32)>& fetch, int& length)
{
	return Disassemble(pc, fetch, length, false);
}

}	// namespace bebox
