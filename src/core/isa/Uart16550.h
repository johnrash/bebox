/*
 * BeBox emulator - National Semiconductor 16550 UART (also the channels of
 * the TI TL16C554 quad UART).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * BeBox serial1/2 are 16550s in the Intel 82091AA (COM1 0x3f8 IRQ4, COM2
 * 0x2f8 IRQ3, 1.8432 MHz). serial3/4 and midi1/2 are TL16C554 channels at
 * 0x380, 0x388, 0x3a0, 0x3a8, clocked at 8 MHz, with their own Be
 * interrupt controller bits.
 */
#ifndef BEBOX_UART_16550_H
#define BEBOX_UART_16550_H

#include "../bus/IoSpace.h"
#include "../board/Scheduler.h"
#include "../host/SerialBackend.h"

#include <deque>
#include <functional>
#include <memory>

namespace bebox {

class Uart16550 : public ByteIoDevice {
public:
	typedef std::function<void(bool)> IrqCallback;
	typedef std::function<void(u8)> TransmitCallback;

								Uart16550(const char* name, u32 base,
									u32 clock, Scheduler& scheduler,
									u64 ticksPerSecond, bool out2GatesIrq);

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				SetBackend(SerialBackend* backend);
			SerialBackend*		Backend() const { return fBackend; }
			// Called with every byte sent on the line (not in loopback
			// mode), with or without a backend.
			void				SetTransmitCallback(TransmitCallback callback)
									{ fTransmitted = callback; }
			// Modem status inputs tied to fixed levels on the board (MSR
			// bits 4-7) when no backend drives them.
			void				SetHardwiredLines(u8 lines)
									{ fHardwiredLines = lines; }
			const char*			Name() const { return fName; }

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return fName; }

private:
			u64					_CharacterTicks() const;
			void				_UpdateIrq();
			void				_StartTransmitter();
			void				_TransmitDone();
			void				_Poll();
			void				_SchedulePoll();
			void				_ReceiveByte(u8 byte);
			void				_LineConfigChanged();
			u8					_ModemStatusLines();
			void				_UpdateModemStatus();

			const char*			fName;
			u32					fBase;
			u32					fClock;
			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			bool				fOut2GatesIrq;
			SerialBackend*		fBackend;
			IrqCallback			fIrq;
			TransmitCallback	fTransmitted;
			u8					fHardwiredLines;

			u16					fDivisor;
			u8					fIer;
			u8					fLcr;
			u8					fMcr;
			u8					fLsr;
			u8					fMsr;
			u8					fScratch;
			u8					fFcr;
			bool				fFifoEnabled;
			std::deque<u8>		fRxFifo;
			std::deque<u8>		fTxFifo;		// or the THR
			bool				fTransmitting;
			u8					fShift;			// the byte being sent
			bool				fThrePending;	// THRE interrupt condition
			bool				fTimeoutPending;
			bool				fIrqLevel;
			std::unique_ptr<Timer> fTxTimer;
			std::unique_ptr<Timer> fPollTimer;
			std::unique_ptr<Timer> fTimeoutTimer;
};

}	// namespace bebox

#endif	// BEBOX_UART_16550_H
