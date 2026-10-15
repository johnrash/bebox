/*
 * BeBox emulator - Intel 8259A programmable interrupt controller.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Pic8259.h"

#include "../Log.h"

namespace bebox {

Pic8259::Pic8259(const char* name)
	:
	fName(name),
	fSlave(nullptr),
	fSlaveLine(-1),
	fLines(0),
	fLevelMask(0)
{
	Reset();
}


void
Pic8259::Reset()
{
	fIrr = 0;
	fIsr = 0;
	fImr = 0xff;
	fVectorBase = 0;
	fPriorityBase = 0;
	fInitStep = 0;
	fNeedIcw4 = false;
	fSingle = false;
	fAutoEoi = false;
	fRotateOnAutoEoi = false;
	fSpecialMask = false;
	fReadIsr = false;
	fPoll = false;
	fOutputLevel = false;
	// level triggered requests follow their lines
	fIrr = fLines & fLevelMask;
}


int
Pic8259::_PriorityOf(int line) const
{
	return (line - fPriorityBase) & 7;	// 0 = highest
}


int
Pic8259::_HighestRequest() const
{
	u8 pending = fIrr & ~fImr;
	int best = -1;
	for (int line = 0; line < 8; line++) {
		if ((pending & (1 << line)) == 0)
			continue;
		if (best < 0 || _PriorityOf(line) < _PriorityOf(best))
			best = line;
	}
	return best;
}


int
Pic8259::_HighestInService() const
{
	u8 service = fIsr;
	if (fSpecialMask)
		service &= ~fImr;
	int best = -1;
	for (int line = 0; line < 8; line++) {
		if ((service & (1 << line)) == 0)
			continue;
		if (best < 0 || _PriorityOf(line) < _PriorityOf(best))
			best = line;
	}
	return best;
}


void
Pic8259::_Update()
{
	bool output = false;
	int request = _HighestRequest();
	if (request >= 0) {
		int service = _HighestInService();
		// A request interrupts only if it has a higher priority than the
		// highest one in service (special fully nested mode for the
		// cascade input on the master is not needed by BeOS).
		if (service < 0 || _PriorityOf(request) < _PriorityOf(service))
			output = true;
	}
	if (output != fOutputLevel) {
		fOutputLevel = output;
		if (fOutput)
			fOutput(output);
	}
}


void
Pic8259::SetLine(int line, bool level)
{
	u8 bit = 1 << line;
	bool old = (fLines & bit) != 0;
	if (level)
		fLines |= bit;
	else
		fLines &= ~bit;
	if ((fLevelMask & bit) != 0) {
		if (level)
			fIrr |= bit;
		else
			fIrr &= ~bit;
	} else if (level && !old) {
		fIrr |= bit;	// rising edge
	} else if (!level) {
		// The 8259A needs the request held until the first INTA; one
		// that goes away before then is forgotten (INTA then returns
		// the spurious IR7 vector).
		fIrr &= ~bit;
	}
	_Update();
}


void
Pic8259::SetLevelTriggered(u8 mask)
{
	fLevelMask = mask;
	fIrr = (fIrr & ~mask) | (fLines & mask);
	_Update();
}


u8
Pic8259::Acknowledge()
{
	int line = _HighestRequest();
	if (line < 0) {
		// spurious interrupt: IR7's vector, no ISR bit
		return fVectorBase | 7;
	}
	u8 bit = 1 << line;
	if ((fLevelMask & bit) == 0)
		fIrr &= ~bit;
	if (!fAutoEoi)
		fIsr |= bit;
	else if (fRotateOnAutoEoi)
		fPriorityBase = (line + 1) & 7;
	u8 vector;
	if (fSlave != nullptr && line == fSlaveLine)
		vector = fSlave->Acknowledge();
	else
		vector = fVectorBase | line;
	_Update();
	return vector;
}


void
Pic8259::_EndOfInterrupt(int line)
{
	if (line >= 0)
		fIsr &= ~(1 << line);
}


u8
Pic8259::Read(int a0)
{
	if (fPoll) {
		fPoll = false;
		int line = _HighestRequest();
		if (line < 0)
			return 0;
		Acknowledge();
		return 0x80 | line;
	}
	if (a0 != 0)
		return fImr;
	return fReadIsr ? fIsr : fIrr;
}


void
Pic8259::Write(int a0, u8 value)
{
	if (a0 == 0 && (value & 0x10) != 0) {
		// ICW1
		fInitStep = 2;
		fNeedIcw4 = (value & 1) != 0;
		fSingle = (value & 2) != 0;
		fImr = 0;
		fIsr = 0;
		fIrr = fLines & fLevelMask;
		fPriorityBase = 0;
		fSpecialMask = false;
		fReadIsr = false;
		fAutoEoi = false;
		_Update();
		return;
	}
	if (a0 == 1 && fInitStep != 0) {
		switch (fInitStep) {
			case 2:
				fVectorBase = value & 0xf8;
				fInitStep = fSingle ? (fNeedIcw4 ? 4 : 0) : 3;
				break;
			case 3:
				fInitStep = fNeedIcw4 ? 4 : 0;
				break;
			case 4:
				fAutoEoi = (value & 2) != 0;
				fInitStep = 0;
				break;
		}
		BB_LOG(kLogPic, kLogDebug, "%s: init step, value %02x (base %02x)",
			fName, value, fVectorBase);
		return;
	}
	if (a0 == 1) {
		// OCW1: interrupt mask
		fImr = value;
		_Update();
		return;
	}
	if ((value & 0x18) == 0) {
		// OCW2
		int command = (value >> 5) & 7;
		int level = value & 7;
		switch (command) {
			case 1:		// non-specific EOI
				_EndOfInterrupt(_HighestInService());
				break;
			case 3:		// specific EOI
				_EndOfInterrupt(level);
				break;
			case 5:		// rotate on non-specific EOI
			{
				int line = _HighestInService();
				_EndOfInterrupt(line);
				if (line >= 0)
					fPriorityBase = (line + 1) & 7;
				break;
			}
			case 4:		// set rotate in automatic EOI mode
				fRotateOnAutoEoi = true;
				break;
			case 0:		// clear rotate in automatic EOI mode
				fRotateOnAutoEoi = false;
				break;
			case 7:		// rotate on specific EOI
				_EndOfInterrupt(level);
				fPriorityBase = (level + 1) & 7;
				break;
			case 6:		// set priority
				fPriorityBase = (level + 1) & 7;
				break;
		}
		_Update();
		return;
	}
	if ((value & 0x18) == 0x08) {
		// OCW3
		if ((value & 2) != 0)
			fReadIsr = (value & 1) != 0;
		if ((value & 4) != 0)
			fPoll = true;
		if ((value & 0x40) != 0)
			fSpecialMask = (value & 0x20) != 0;
		_Update();
	}
}

}	// namespace bebox
