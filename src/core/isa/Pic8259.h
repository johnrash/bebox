/*
 * BeBox emulator - Intel 8259A programmable interrupt controller.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_PIC_8259_H
#define BEBOX_PIC_8259_H

#include "../Types.h"

#include <functional>

namespace bebox {

class Pic8259 {
public:
	typedef std::function<void(bool)> OutputCallback;

								Pic8259(const char* name);

			void				Reset();
			void				SetOutputCallback(OutputCallback callback)
									{ fOutput = callback; }
			// The slave connected to this master's cascade input (IR2).
			void				SetSlave(Pic8259* slave, int line)
									{ fSlave = slave; fSlaveLine = line; }

			u8					Read(int a0);
			void				Write(int a0, u8 value);

			// Interrupt request input level (devices drive this).
			void				SetLine(int line, bool level);
			// Level triggered lines (ELCR of the 82378).
			void				SetLevelTriggered(u8 mask);
			u8					LevelTriggered() const { return fLevelMask; }

			bool				Output() const { return fOutputLevel; }
			// Interrupt acknowledge sequence: returns the vector.
			u8					Acknowledge();

			u8					Irr() const { return fIrr; }
			u8					Isr() const { return fIsr; }
			u8					Imr() const { return fImr; }

private:
			int					_PriorityOf(int line) const;
			int					_HighestRequest() const;
			int					_HighestInService() const;
			void				_Update();
			void				_EndOfInterrupt(int line);

			const char*			fName;
			OutputCallback		fOutput;
			Pic8259*			fSlave;
			int					fSlaveLine;

			u8					fIrr;
			u8					fIsr;
			u8					fImr;
			u8					fLines;			// input levels
			u8					fLevelMask;
			u8					fVectorBase;
			u8					fPriorityBase;	// lowest priority + 1
			int					fInitStep;		// 0: operational
			bool				fNeedIcw4;
			bool				fSingle;
			bool				fAutoEoi;
			bool				fRotateOnAutoEoi;
			bool				fSpecialMask;
			bool				fReadIsr;
			bool				fPoll;
			bool				fOutputLevel;
};

}	// namespace bebox

#endif	// BEBOX_PIC_8259_H
