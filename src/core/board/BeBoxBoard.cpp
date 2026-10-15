/*
 * BeBox emulator - BeBox motherboard logic.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "BeBoxBoard.h"

#include "Scheduler.h"
#include "../Log.h"
#include "../cpu/PpcCpu.h"

namespace bebox {

static inline u32
Bit(int ibmBit)
{
	return 0x80000000u >> ibmBit;
}


BeBoxBoard::BeBoxBoard(PhysBus& bus, IoSpace& io, Scheduler& scheduler)
	:
	fBus(bus),
	fScheduler(scheduler),
	fSources(0),
	fInterruptSwitch(false)
{
	fCpus[0] = fCpus[1] = nullptr;
	bus.MapDevice(kRegisterPage, 0x1000, this);
	io.Map(0xc00, 1, this);
	PowerOnReset();
}


void
BeBoxBoard::AttachCpus(PpcCpu* cpu0, PpcCpu* cpu1)
{
	fCpus[0] = cpu0;
	fCpus[1] = cpu1;
	_UpdateCrossProcessor();
	_UpdateResets(~fResets);
	_Update();
}


void
BeBoxBoard::PowerOnReset()
{
	fMask[0] = fMask[1] = 0;
	// /TLBISYNC and the CPU1 resets are asserted (0) until the nub
	// changes them; no SMIs.
	fCrossProcessor = 0;
	fResets = 0;
	fLedLatch = 0;
	fLedLastChange = fScheduler.Now();
	fLedWindowStart = fLedLastChange;
	fLedLeftSum = fLedRightSum = 0;
	fDiskActivity = false;
	_UpdateCrossProcessor();
	_UpdateResets(~fResets);
	_Update();
}


int
BeBoxBoard::SourceForIsaIrq(int irq)
{
	switch (irq) {
		case 1: return kBeIrqKeyboard;
		case 3: return kBeIrqSerial2;
		case 4: return kBeIrqSerial1;
		case 5: return kBeIrqIsa5;
		case 6: return kBeIrqFloppy;
		case 7: return kBeIrqParallel;
		case 8: return kBeIrqRtc;
		case 9: return kBeIrqIsa9;
		case 10: return kBeIrqIsa10;
		case 11: return kBeIrqIsa11;
		case 12: return kBeIrqMouse;
		case 14: return kBeIrqIde;
		case 15: return kBeIrqIsa15;
	}
	return -1;
}


void
BeBoxBoard::SetSource(int bit, bool level)
{
	u32 mask = Bit(bit);
	u32 old = fSources;
	if (level)
		fSources |= mask;
	else
		fSources &= ~mask;
	if (fSources != old)
		_Update();
}


void
BeBoxBoard::_Update()
{
	for (int cpu = 0; cpu < 2; cpu++) {
		if (fCpus[cpu] == nullptr)
			continue;
		bool asserted = (fSources & fMask[cpu] & 0x0ffffffcu) != 0;
		fCpus[cpu]->SetIntLine(asserted);
	}
}


u32
BeBoxBoard::_Apply(u32 current, u32 value)
{
	u32 bits = value & 0x7fffffff;
	return (value & 0x80000000) != 0 ? current | bits : current & ~bits;
}


void
BeBoxBoard::SetInterruptSwitch(bool pressed)
{
	fInterruptSwitch = pressed;
	if (pressed)
		fSources |= Bit(1) | Bit(2);
	else
		fSources &= ~(Bit(1) | Bit(2));
	_UpdateCrossProcessor();
}


void
BeBoxBoard::_UpdateCrossProcessor()
{
	bool switch0 = fInterruptSwitch && (fMask[0] & Bit(1)) != 0;
	bool switch1 = fInterruptSwitch && (fMask[1] & Bit(2)) != 0;
	if (fCpus[0] != nullptr) {
		fCpus[0]->SetSmiLine((fCrossProcessor & Bit(1)) != 0 || switch0);
		fCpus[0]->SetTlbisyncLine((fCrossProcessor & Bit(4)) == 0);
	}
	if (fCpus[1] != nullptr) {
		fCpus[1]->SetSmiLine((fCrossProcessor & Bit(2)) != 0 || switch1);
		fCpus[1]->SetTlbisyncLine((fCrossProcessor & Bit(5)) == 0);
	}
}


void
BeBoxBoard::_UpdateResets(u32 old)
{
	if (fCpus[1] == nullptr)
		return;
	u32 changed = old ^ fResets;
	if ((changed & Bit(2)) != 0) {
		bool hreset = (fResets & Bit(2)) == 0;
		BB_LOG(kLogBoard, kLogInfo, "CPU1 HRESET %s",
			hreset ? "asserted" : "released");
		fCpus[1]->SetNow(fScheduler.Now());
		fCpus[1]->SetHresetLine(hreset);
	}
	if ((changed & Bit(1)) != 0 && (fResets & Bit(1)) == 0) {
		BB_LOG(kLogBoard, kLogInfo, "CPU1 SRESET asserted");
		fCpus[1]->SetSresetLine(true);
	}
}


void
BeBoxBoard::_AccumulateLeds()
{
	u64 now = fScheduler.Now();
	if (now > fLedLastChange) {
		double span = (double)(now - fLedLastChange);
		fLedLeftSum += span * (fLedLatch >> 4);
		fLedRightSum += span * (fLedLatch & 15);
	}
	fLedLastChange = now;
}


void
BeBoxBoard::SampleLeds(float& left, float& right, float& disk)
{
	_AccumulateLeds();
	u64 now = fScheduler.Now();
	double span = (double)(now - fLedWindowStart);
	if (span <= 0) {
		left = fLedLatch >> 4;
		right = fLedLatch & 15;
	} else {
		left = (float)(fLedLeftSum / span);
		right = (float)(fLedRightSum / span);
	}
	disk = fDiskActivity || DiskLed() ? 1 : 0;
	fLedWindowStart = now;
	fLedLeftSum = fLedRightSum = 0;
	fDiskActivity = false;
}


bool
BeBoxBoard::CpuActive(int cpu) const
{
	return fCpus[cpu] != nullptr && !fCpus[cpu]->IsHeldInReset();
}


u32
BeBoxBoard::MmioRead(u32 addr, int size, int initiator)
{
	u32 reg = addr & 0xfff;
	u32 value;
	switch (reg & 0xff8) {
		case 0x0f0: value = fMask[0]; break;
		case 0x1f0: value = fMask[1]; break;
		case 0x2f0: value = fSources; break;
		case 0x3f0:
			value = fCrossProcessor & ~Bit(6);
			if (initiator == kInitiatorCpu1)
				value |= Bit(6);
			break;
		case 0x4f0: value = fResets; break;
		default:
			BB_LOG(kLogBoard, kLogInfo, "read of unknown register %08x", addr);
			return 0;
	}
	// The registers sit in the upper half of a 64-bit doubleword.
	if ((reg & 4) != 0)
		return 0;
	u32 offset = reg & 3;
	if (size == 4)
		return value;
	if (size == 2)
		return (value >> (16 - 8 * offset)) & 0xffff;
	return (value >> (24 - 8 * offset)) & 0xff;
}


void
BeBoxBoard::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	u32 reg = addr & 0xfff;
	if (size != 4 || (reg & 7) != 0) {
		BB_LOG(kLogBoard, kLogWarning, "unsupported write%d at %08x", size * 8,
			addr);
		return;
	}
	switch (reg) {
		case 0x0f0:
			fMask[0] = _Apply(fMask[0], value);
			_Update();
			_UpdateCrossProcessor();
			break;
		case 0x1f0:
			fMask[1] = _Apply(fMask[1], value);
			_Update();
			_UpdateCrossProcessor();
			break;
		case 0x3f0:
			fCrossProcessor = _Apply(fCrossProcessor, value) & ~Bit(6);
			_UpdateCrossProcessor();
			break;
		case 0x4f0:
		{
			u32 old = fResets;
			_AccumulateLeds();
			fResets = _Apply(fResets, value);
			if (DiskLed())
				fDiskActivity = true;
			_UpdateResets(old);
			break;
		}
		default:
			BB_LOG(kLogBoard, kLogInfo, "write to register %08x: %08x", addr,
				value);
			break;
	}
}


u8
BeBoxBoard::IoRead8(u32 port)
{
	return fLedLatch;
}


void
BeBoxBoard::IoWrite8(u32 port, u8 value)
{
	_AccumulateLeds();
	fLedLatch = value;
}

}	// namespace bebox
