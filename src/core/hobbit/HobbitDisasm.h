/*
 * BeBox emulator - AT&T ATT92010 "Hobbit" disassembler (traces, debugging).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_HOBBIT_DISASM_H
#define BEBOX_HOBBIT_DISASM_H

#include "../Types.h"

#include <functional>
#include <string>

namespace bebox {

// Disassembles the instruction at 'pc' (a CPU prefix together with the
// instruction it prefixes). 'fetch' reads a parcel. Returns the text and
// sets 'length' to its size in bytes.
std::string HobbitDisassemble(u32 pc, const std::function<u16(u32)>& fetch,
	int& length);

}	// namespace bebox

#endif	// BEBOX_HOBBIT_DISASM_H
