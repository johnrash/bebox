/*
 * BeBox emulator - the Hobbit BeBox board ("Sako"): I/O window, interrupt
 * controller, processor resets.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "HobbitBoard.h"

#include "HobbitCpu.h"
#include "../Log.h"

#include <algorithm>
#include <cstring>

namespace bebox {

// Board register ports (lane 3)
enum {
	kPortIoInterrupts = 0,
	kPortIoMask1 = 1,
	kPortIoMask2 = 2,
	kPortSlotInterrupts = 3,
	kPortSlotMask1 = 4,
	kPortSlotMask2 = 5,
	kPortResets = 6,
	kPortInternal = 7,
	kPortWhoAmI = 0x370
};

// SCSI controller registers (lane 2): 0x200 + register, 0x210 pseudo-DMA
static const u32 kScsiFirstPort = 0x200;
static const u32 kScsiLastPort = 0x210;

// Bus master numbers as "who am I" reports them.
static const u8 kMasterCpu1 = 4;

// The floppy DMA registers (offsets in the window)
static const u32 kDmaFirst = 0x20;
static const u32 kDmaLast = 0x2f;

// The floppy controller: at 0x370 here, at 0x3f0 in the emulated chip
static const u32 kFloppyFirstPort = 0x371;
static const u32 kFloppyLastPort = 0x377;
static const u32 kFloppyPortOffset = 0x80;


HobbitBoard::HobbitBoard(PhysBus& bus, IoSpace& io)
	:
	fBus(bus),
	fIo(io),
	fScsi(nullptr),
	fIoPending(0),
	fSlotPending(0),
	fResets(0),
	fInternal(0),
	fNmi(false),
	fDmaAddress(0),
	fDmaRemaining(0),
	fDmaFromMemory(false)
{
	memset(fDmaRegisters, 0, sizeof(fDmaRegisters));
	fCpus[0] = fCpus[1] = nullptr;
	fIoMask[0] = fIoMask[1] = 0;
	fSlotMask[0] = fSlotMask[1] = 0;
}


void
HobbitBoard::AttachCpus(HobbitCpu* cpu0, HobbitCpu* cpu1)
{
	fCpus[0] = cpu0;
	fCpus[1] = cpu1;
}


void
HobbitBoard::PowerOnReset()
{
	fIoMask[0] = fIoMask[1] = 0;
	fSlotMask[0] = fSlotMask[1] = 0;
	fInternal = 0;
	fResets = 0x01;
	for (int i = 0; i < 2; i++) {
		if (fCpus[i] != nullptr)
			fCpus[i]->SetHeldInReset((fResets & (1 << i)) == 0);
	}
	_UpdateInterrupts();
}


void
HobbitBoard::SetIoInterrupt(int bit, bool level)
{
	u8 mask = 1 << bit;
	u8 pending = level ? fIoPending | mask : fIoPending & ~mask;
	if (pending == fIoPending)
		return;
	fIoPending = pending;
	_UpdateInterrupts();
}


void
HobbitBoard::SetSlotInterrupt(int slot, bool level)
{
	u8 mask = 1 << slot;
	fSlotPending = level ? fSlotPending | mask : fSlotPending & ~mask;
	_UpdateInterrupts();
}


void
HobbitBoard::SetNmi(bool level)
{
	fNmi = level;
	_UpdateInterrupts();
}


bool
HobbitBoard::CpuRunning(int cpu) const
{
	return (fResets & (1 << cpu)) != 0;
}


// Each CPU sees the highest priority (lowest) level among its sources.
void
HobbitBoard::_UpdateInterrupts()
{
	for (int i = 0; i < 2; i++) {
		if (fCpus[i] == nullptr)
			continue;
		int level = 7;
		if ((fSlotPending & fSlotMask[i]) != 0)
			level = 5;
		if ((fIoPending & fIoMask[i]) != 0)
			level = 3;
		if ((fInternal & (1 << i)) != 0)
			level = 1;
		if (i == 0 && fNmi)
			level = 0;
		fCpus[i]->SetInterruptLevel(level);
	}
}


void
HobbitBoard::_SetResets(u8 value)
{
	u8 released = value & ~fResets;
	fResets = value & 3;
	for (int i = 0; i < 2; i++) {
		if (fCpus[i] == nullptr)
			continue;
		bool held = (fResets & (1 << i)) == 0;
		if (held != fCpus[i]->IsHeldInReset()) {
			BB_LOG(kLogBoard, kLogInfo, "CPU %d %s", i + 1,
				held ? "held in reset" : "released from reset");
		}
		fCpus[i]->SetHeldInReset(held);
		if ((released & (1 << i)) != 0) {
			fCpus[i]->Reset();
			if (fRelease)
				fRelease(i);
		}
	}
}


u8
HobbitBoard::_Read8(u32 offset, int initiator)
{
	u32 port = offset >> 2;
	int lane = offset & 3;
	if (offset >= kDmaFirst && offset <= kDmaLast)
		return fDmaRegisters[offset - kDmaFirst];
	if (lane == 2 && port >= kScsiFirstPort && port <= kScsiLastPort
		&& fScsi != nullptr)
		return (u8)fScsi->IoRead(port - kScsiFirstPort, 1);
	if (lane != 3) {
		BB_LOG(kLogBus, kLogDebug, "hobbit-io: read of lane %d at %08x",
			lane, kIoBase + offset);
		return 0xff;
	}
	switch (port) {
		case kPortIoInterrupts:
			return fIoPending;
		case kPortIoMask1:
			return fIoMask[0];
		case kPortIoMask2:
			return fIoMask[1];
		case kPortSlotInterrupts:
			return fSlotPending;
		case kPortSlotMask1:
			return fSlotMask[0];
		case kPortSlotMask2:
			return fSlotMask[1];
		case kPortResets:
			return fResets;
		case kPortInternal:
			return fInternal;
		case kPortWhoAmI:
		{
			// the current bus master; the joystick buttons (not pressed)
			int master = initiator == kInitiatorCpu1 ? 1 : 0;
			return 0xf0 | (kMasterCpu1 + master);
		}
	}
	if (port >= kFloppyFirstPort && port <= kFloppyLastPort)
		port += kFloppyPortOffset;
	return (u8)fIo.Read(port, 1, initiator);
}


void
HobbitBoard::_Write8(u32 offset, u8 value, int initiator)
{
	u32 port = offset >> 2;
	int lane = offset & 3;
	if (offset >= kDmaFirst && offset <= kDmaLast) {
		fDmaRegisters[offset - kDmaFirst] = value;
		if (offset == kDmaFirst + 3) {
			// the count's last byte: the transfer is set up
			fDmaRemaining = LoadBE32(fDmaRegisters) + 1;
			fDmaFromMemory = (LoadBE32(fDmaRegisters + 4) & 1) != 0;
			fDmaAddress = ((u32)LoadBE16(fDmaRegisters + 0xa) << 16)
				| (LoadBE16(fDmaRegisters + 0xe) & 0xfffc);
			BB_LOG(kLogDma, kLogDebug, "floppy DMA: %u bytes %s %08x",
				fDmaRemaining, fDmaFromMemory ? "from" : "to", fDmaAddress);
		}
		return;
	}
	if (lane == 2 && port >= kScsiFirstPort && port <= kScsiLastPort
		&& fScsi != nullptr) {
		fScsi->IoWrite(port - kScsiFirstPort, value, 1);
		return;
	}
	if (lane != 3) {
		// word accesses to lane 3 registers also write lanes 0-2
		return;
	}
	switch (port) {
		case kPortIoInterrupts:
			return;
		case kPortIoMask1:
			fIoMask[0] = value;
			_UpdateInterrupts();
			return;
		case kPortIoMask2:
			fIoMask[1] = value;
			_UpdateInterrupts();
			return;
		case kPortSlotInterrupts:
			return;
		case kPortSlotMask1:
			fSlotMask[0] = value;
			_UpdateInterrupts();
			return;
		case kPortSlotMask2:
			fSlotMask[1] = value;
			_UpdateInterrupts();
			return;
		case kPortResets:
			_SetResets(value);
			return;
		case kPortInternal:
			fInternal = value & 0x1f;
			_UpdateInterrupts();
			return;
		case kPortWhoAmI:
			return;
	}
	if (port >= kFloppyFirstPort && port <= kFloppyLastPort)
		port += kFloppyPortOffset;
	fIo.Write(port, value, 1, initiator);
}


u32
HobbitBoard::Transfer(int channel, void* buffer, u32 length,
	bool& terminalCount)
{
	terminalCount = false;
	if (channel != 2 || fDmaRemaining == 0)
		return 0;
	u32 count = std::min(length, fDmaRemaining);
	if (fDmaFromMemory)
		fBus.DmaRead(fDmaAddress, buffer, count);
	else
		fBus.DmaWrite(fDmaAddress, buffer, count);
	fDmaAddress += count;
	fDmaRemaining -= count;
	terminalCount = fDmaRemaining == 0;
	return count;
}


bool
HobbitBoard::IsMasked(int channel) const
{
	return channel != 2 || fDmaRemaining == 0;
}


u32
HobbitBoard::MmioRead(u32 address, int size, int initiator)
{
	u32 offset = address - kIoBase;
	// the SCSI controller's 16-bit pseudo-DMA port
	if (size == 2 && offset == kScsiLastPort * 4 + 2 && fScsi != nullptr)
		return fScsi->IoRead(kScsiLastPort - kScsiFirstPort, 2);
	u32 value = 0;
	for (int i = 0; i < size; i++)
		value = (value << 8) | _Read8(offset + i, initiator);
	return value;
}


void
HobbitBoard::MmioWrite(u32 address, u32 value, int size, int initiator)
{
	u32 offset = address - kIoBase;
	if (size == 2 && offset == kScsiLastPort * 4 + 2 && fScsi != nullptr) {
		fScsi->IoWrite(kScsiLastPort - kScsiFirstPort, value, 2);
		return;
	}
	for (int i = 0; i < size; i++)
		_Write8(offset + i, (u8)(value >> (8 * (size - 1 - i))), initiator);
}

}	// namespace bebox
