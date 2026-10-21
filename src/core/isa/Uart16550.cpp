/*
 * BeBox emulator - 16550 UART.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Uart16550.h"

#include "../Log.h"

namespace bebox {

enum {
	kIerRx = 0x01, kIerThre = 0x02, kIerLsr = 0x04, kIerMsr = 0x08,
	kLcrDlab = 0x80, kLcrBreak = 0x40,
	kMcrDtr = 0x01, kMcrRts = 0x02, kMcrOut1 = 0x04, kMcrOut2 = 0x08,
	kMcrLoop = 0x10,
	kLsrDr = 0x01, kLsrOe = 0x02, kLsrPe = 0x04, kLsrFe = 0x08,
	kLsrBi = 0x10, kLsrThre = 0x20, kLsrTemt = 0x40, kLsrFifoError = 0x80,
	kMsrDcts = 0x01, kMsrDdsr = 0x02, kMsrTeri = 0x04, kMsrDdcd = 0x08,
	kMsrCts = 0x10, kMsrDsr = 0x20, kMsrRi = 0x40, kMsrDcd = 0x80
};

static const size_t kFifoSize = 16;


Uart16550::Uart16550(const char* name, u32 base, u32 clock,
	Scheduler& scheduler, u64 ticksPerSecond, bool out2GatesIrq)
	:
	fName(name),
	fBase(base),
	fClock(clock),
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fOut2GatesIrq(out2GatesIrq),
	fBackend(nullptr),
	fHardwiredLines(0),
	fIrqLevel(false)
{
	fTxTimer.reset(new Timer(scheduler, [this]() { _TransmitDone(); },
		"uart-tx"));
	fPollTimer.reset(new Timer(scheduler, [this]() { _Poll(); },
		"uart-rx"));
	fTimeoutTimer.reset(new Timer(scheduler, [this]() {
		if (fFifoEnabled && !fRxFifo.empty()) {
			fTimeoutPending = true;
			_UpdateIrq();
		}
	}, "uart-timeout"));
	Reset();
}


void
Uart16550::Reset()
{
	fDivisor = 12;
	fIer = 0;
	fLcr = 0x03;
	fMcr = 0;
	fLsr = kLsrThre | kLsrTemt;
	fMsr = 0;
	fScratch = 0;
	fFcr = 0;
	fFifoEnabled = false;
	fRxFifo.clear();
	fTxFifo.clear();
	fTransmitting = false;
	fShift = 0;
	fThrePending = false;
	fTimeoutPending = false;
	fTxTimer->Cancel();
	fTimeoutTimer->Cancel();
	_UpdateModemStatus();
	fMsr &= 0xf0;	// no deltas after reset
	_UpdateIrq();
	_SchedulePoll();
}


void
Uart16550::SetBackend(SerialBackend* backend)
{
	fBackend = backend;
	_LineConfigChanged();
	_SchedulePoll();
}


u64
Uart16550::_CharacterTicks() const
{
	u32 divisor = fDivisor == 0 ? 0x10000 : fDivisor;
	u64 bitsPerChar = 1 + (5 + (fLcr & 3)) + ((fLcr & 8) != 0 ? 1 : 0)
		+ ((fLcr & 4) != 0 ? 2 : 1);
	// bit time = 16 * divisor / clock
	return (u64)(((unsigned __int128)fTicksPerSecond * 16 * divisor
		* bitsPerChar) / fClock);
}


void
Uart16550::_UpdateIrq()
{
	bool pending = false;
	if ((fIer & kIerLsr) != 0 && (fLsr & (kLsrOe | kLsrPe | kLsrFe | kLsrBi)))
		pending = true;
	if ((fIer & kIerRx) != 0 && ((fLsr & kLsrDr) != 0 || fTimeoutPending))
		pending = true;
	if ((fIer & kIerThre) != 0 && fThrePending)
		pending = true;
	if ((fIer & kIerMsr) != 0 && (fMsr & 0x0f) != 0)
		pending = true;
	bool level = pending;
	if (fOut2GatesIrq && (fMcr & kMcrOut2) == 0)
		level = false;
	if ((fMcr & kMcrLoop) != 0 && fOut2GatesIrq)
		level = false;	// in loopback the IRQ output is disconnected
	if (level != fIrqLevel) {
		fIrqLevel = level;
		if (fIrq)
			fIrq(level);
	}
}


// The next byte moves from the THR (or FIFO) to the transmitter shift
// register at once, as on the chip: the THR is free again while the byte
// is sent (so two bytes fit without a FIFO).
void
Uart16550::_StartTransmitter()
{
	if (fTransmitting || fTxFifo.empty())
		return;
	fShift = fTxFifo.front();
	fTxFifo.pop_front();
	fTransmitting = true;
	fLsr &= ~kLsrTemt;
	if (fTxFifo.empty()) {
		fLsr |= kLsrThre;
		fThrePending = true;
		_UpdateIrq();
	}
	fTxTimer->ArmIn(_CharacterTicks());
}


void
Uart16550::_TransmitDone()
{
	fTransmitting = false;
	if ((fMcr & kMcrLoop) != 0) {
		_ReceiveByte(fShift);
	} else {
		if (fBackend != nullptr)
			fBackend->Transmit(fShift);
		if (fTransmitted)
			fTransmitted(fShift);
	}
	if (fTxFifo.empty())
		fLsr |= kLsrTemt;
	else
		_StartTransmitter();
}


void
Uart16550::_ReceiveByte(u8 byte)
{
	size_t limit = fFifoEnabled ? kFifoSize : 1;
	if (fRxFifo.size() >= limit) {
		fLsr |= kLsrOe;
		if (!fFifoEnabled) {
			fRxFifo.clear();
			fRxFifo.push_back(byte);	// the holding register is overwritten
		}
	} else {
		fRxFifo.push_back(byte);
	}
	fLsr |= kLsrDr;
	if (fFifoEnabled) {
		static const size_t kTriggers[4] = { 1, 4, 8, 14 };
		size_t trigger = kTriggers[(fFcr >> 6) & 3];
		// Below the trigger level only the character timeout interrupts.
		fTimeoutTimer->ArmIn(4 * _CharacterTicks());
		if (fRxFifo.size() < trigger) {
			_UpdateIrq();
			return;
		}
	}
	_UpdateIrq();
}


void
Uart16550::_SchedulePoll()
{
	if (fBackend == nullptr) {
		fPollTimer->Cancel();
		return;
	}
	if (!fPollTimer->IsArmed())
		fPollTimer->ArmIn(_CharacterTicks());
}


void
Uart16550::_Poll()
{
	if (fBackend == nullptr)
		return;
	if ((fMcr & kMcrLoop) == 0) {
		size_t limit = fFifoEnabled ? kFifoSize : 1;
		if (fRxFifo.size() < limit) {
			int byte = fBackend->Receive();
			if (byte >= 0)
				_ReceiveByte((u8)byte);
		}
		_UpdateModemStatus();
		_UpdateIrq();
	}
	// One character time per byte; idle lines are polled every 2 ms.
	u64 interval = _CharacterTicks();
	if (fRxFifo.empty() && interval < fTicksPerSecond / 500)
		interval = fTicksPerSecond / 500;
	fPollTimer->ArmIn(interval);
}


void
Uart16550::_LineConfigChanged()
{
	if (fBackend == nullptr)
		return;
	SerialLineConfig config;
	u32 divisor = fDivisor == 0 ? 0x10000 : fDivisor;
	config.baud = fClock / (16 * divisor);
	config.dataBits = 5 + (fLcr & 3);
	config.stopBits = (fLcr & 4) != 0 ? 2 : 1;
	if ((fLcr & 8) == 0)
		config.parity = 'N';
	else if ((fLcr & 0x20) != 0)
		config.parity = (fLcr & 0x10) != 0 ? 'S' : 'M';
	else
		config.parity = (fLcr & 0x10) != 0 ? 'E' : 'O';
	fBackend->SetLineConfig(config);
}


u8
Uart16550::_ModemStatusLines()
{
	if ((fMcr & kMcrLoop) != 0) {
		return ((fMcr & kMcrRts) != 0 ? kMsrCts : 0)
			| ((fMcr & kMcrDtr) != 0 ? kMsrDsr : 0)
			| ((fMcr & kMcrOut1) != 0 ? kMsrRi : 0)
			| ((fMcr & kMcrOut2) != 0 ? kMsrDcd : 0);
	}
	if (fBackend == nullptr)
		return fHardwiredLines;
	SerialModemStatus status = fBackend->ModemStatus();
	return (status.cts ? kMsrCts : 0) | (status.dsr ? kMsrDsr : 0)
		| (status.ri ? kMsrRi : 0) | (status.dcd ? kMsrDcd : 0);
}


void
Uart16550::_UpdateModemStatus()
{
	u8 lines = _ModemStatusLines();
	u8 old = fMsr & 0xf0;
	u8 deltas = fMsr & 0x0f;
	if (((lines ^ old) & kMsrCts) != 0)
		deltas |= kMsrDcts;
	if (((lines ^ old) & kMsrDsr) != 0)
		deltas |= kMsrDdsr;
	if ((old & kMsrRi) != 0 && (lines & kMsrRi) == 0)
		deltas |= kMsrTeri;
	if (((lines ^ old) & kMsrDcd) != 0)
		deltas |= kMsrDdcd;
	fMsr = lines | deltas;
}


u8
Uart16550::IoRead8(u32 port)
{
	switch (port - fBase) {
		case 0:
		{
			if ((fLcr & kLcrDlab) != 0)
				return (u8)fDivisor;
			u8 value = 0;
			if (!fRxFifo.empty()) {
				value = fRxFifo.front();
				fRxFifo.pop_front();
			}
			if (fRxFifo.empty())
				fLsr &= ~kLsrDr;
			fTimeoutPending = false;
			if (!fRxFifo.empty() && fFifoEnabled)
				fTimeoutTimer->ArmIn(4 * _CharacterTicks());
			_UpdateIrq();
			return value;
		}
		case 1:
			if ((fLcr & kLcrDlab) != 0)
				return (u8)(fDivisor >> 8);
			return fIer;
		case 2:
		{
			u8 id = 0x01;	// no interrupt pending
			if ((fIer & kIerLsr) != 0
				&& (fLsr & (kLsrOe | kLsrPe | kLsrFe | kLsrBi)) != 0)
				id = 0x06;
			else if ((fIer & kIerRx) != 0 && fTimeoutPending)
				id = 0x0c;
			else if ((fIer & kIerRx) != 0 && (fLsr & kLsrDr) != 0) {
				static const size_t kTriggers[4] = { 1, 4, 8, 14 };
				if (!fFifoEnabled
					|| fRxFifo.size() >= kTriggers[(fFcr >> 6) & 3])
					id = 0x04;
				else if ((fIer & kIerThre) != 0 && fThrePending)
					id = 0x02;
				else if ((fIer & kIerMsr) != 0 && (fMsr & 0x0f) != 0)
					id = 0x00;
			} else if ((fIer & kIerThre) != 0 && fThrePending) {
				id = 0x02;
				fThrePending = false;	// cleared by reading IIR
			} else if ((fIer & kIerMsr) != 0 && (fMsr & 0x0f) != 0)
				id = 0x00;
			if (fFifoEnabled)
				id |= 0xc0;
			_UpdateIrq();
			return id;
		}
		case 3:
			return fLcr;
		case 4:
			return fMcr;
		case 5:
		{
			u8 value = fLsr;
			if (fFifoEnabled && fRxFifo.size() > 0 && (fLsr & 0x1e) != 0)
				value |= kLsrFifoError;
			fLsr &= ~(kLsrOe | kLsrPe | kLsrFe | kLsrBi);
			_UpdateIrq();
			return value;
		}
		case 6:
		{
			_UpdateModemStatus();
			u8 value = fMsr;
			fMsr &= 0xf0;
			_UpdateIrq();
			return value;
		}
		case 7:
			return fScratch;
	}
	return 0xff;
}


void
Uart16550::IoWrite8(u32 port, u8 value)
{
	switch (port - fBase) {
		case 0:
			if ((fLcr & kLcrDlab) != 0) {
				fDivisor = (fDivisor & 0xff00) | value;
				_LineConfigChanged();
				return;
			}
			if (fTxFifo.size() < (fFifoEnabled ? kFifoSize : 1))
				fTxFifo.push_back(value);
			fLsr &= ~(kLsrThre | kLsrTemt);
			fThrePending = false;
			_StartTransmitter();
			_UpdateIrq();
			return;
		case 1:
			if ((fLcr & kLcrDlab) != 0) {
				fDivisor = (fDivisor & 0x00ff) | (value << 8);
				_LineConfigChanged();
				return;
			}
			// Enabling the THRE interrupt with an empty transmitter
			// raises it at once.
			if ((value & kIerThre) != 0 && (fIer & kIerThre) == 0
				&& (fLsr & kLsrThre) != 0)
				fThrePending = true;
			fIer = value & 0x0f;
			_UpdateIrq();
			return;
		case 2:
			fFcr = value;
			fFifoEnabled = (value & 1) != 0;
			if ((value & 2) != 0 || !fFifoEnabled) {
				fRxFifo.clear();
				fLsr &= ~kLsrDr;
				fTimeoutPending = false;
			}
			if ((value & 4) != 0 || !fFifoEnabled) {
				fTxFifo.clear();
				fLsr |= kLsrThre;
				if (!fTransmitting)
					fLsr |= kLsrTemt;
			}
			_UpdateIrq();
			return;
		case 3:
		{
			bool breakChanged = ((fLcr ^ value) & kLcrBreak) != 0;
			fLcr = value;
			if (breakChanged && fBackend != nullptr)
				fBackend->SetBreak((value & kLcrBreak) != 0);
			_LineConfigChanged();
			return;
		}
		case 4:
		{
			u8 old = fMcr;
			fMcr = value & 0x1f;
			if (fBackend != nullptr && ((old ^ fMcr) & 3) != 0)
				fBackend->SetModemControl((fMcr & kMcrDtr) != 0,
					(fMcr & kMcrRts) != 0);
			_UpdateModemStatus();
			_UpdateIrq();
			return;
		}
		case 5:
		case 6:
			return;
		case 7:
			fScratch = value;
			return;
	}
}

}	// namespace bebox
