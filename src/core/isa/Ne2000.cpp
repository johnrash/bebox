/*
 * BeBox emulator - NE2000 compatible ISA Ethernet card.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Ne2000.h"

#include "../Log.h"
#include "../host/NetworkBackend.h"

#include <algorithm>
#include <cstring>

namespace bebox {

namespace {

enum {
	// CR
	kCommandStop = 0x01,
	kCommandStart = 0x02,
	kCommandTransmit = 0x04,
	// ISR
	kIsrReceived = 0x01,
	kIsrTransmitted = 0x02,
	kIsrRemoteDmaDone = 0x40,
	kIsrReset = 0x80,
	// RCR
	kRcrBroadcast = 0x04,
	kRcrMulticast = 0x08,
	kRcrPromiscuous = 0x10,
	// TCR
	kTcrLoopback = 0x06,

	kRamStart = 0x4000,
	kRamEnd = 0x8000,
	kMinFrame = 60
};

}	// namespace


Ne2000::Ne2000(Scheduler& scheduler, u64 ticksPerSecond, const u8 mac[6])
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fBackend(nullptr),
	fIrqLevel(false)
{
	memcpy(fMac, mac, 6);
	// Station address PROM as read through byte wide remote DMA: each byte
	// twice (the PROM sits on both lanes of the 16-bit bus); 57h 57h at
	// 14-15 identifies a 16-bit NE2000.
	memset(fProm, 0, sizeof(fProm));
	for (int i = 0; i < 16; i++) {
		u8 value = i < 6 ? mac[i] : i >= 14 ? 0x57 : 0;
		fProm[i * 2] = fProm[i * 2 + 1] = value;
	}
	fPollTimer.reset(new Timer(scheduler, [this]() { _Poll(); },
		"ne2000"));
	Reset();
}


Ne2000::~Ne2000()
{
}


void
Ne2000::Reset()
{
	fCommand = kCommandStop | 0x20;
	fPageStart = 0x46;
	fPageStop = 0x80;
	fBoundary = 0x46;
	fCurrent = 0x47;
	fTransmitPage = 0x40;
	fTransmitCount = 0;
	fIsr = kIsrReset;
	fImr = 0;
	fRemoteAddress = 0;
	fRemoteCount = 0;
	fRcr = 0;
	fTcr = 0;
	fDcr = 0;
	fTsr = 0;
	fRsr = 0;
	memcpy(fPar, fMac, 6);
	memset(fMar, 0, sizeof(fMar));
	memset(fCounters, 0, sizeof(fCounters));
	_UpdateIrq();
}


void
Ne2000::SetBackend(NetworkBackend* backend)
{
	fBackend = backend;
	if (backend == nullptr) {
		fPollTimer->Cancel();
		return;
	}
	backend->SetReceiver([this](const u8* frame, size_t length) {
		return Receive(frame, length);
	});
	fPollTimer->ArmIn(fTicksPerSecond / 1000);
}


// The host side is serviced every millisecond of emulated time.
void
Ne2000::_Poll()
{
	if (fBackend == nullptr)
		return;
	fBackend->Poll();
	fPollTimer->ArmIn(fTicksPerSecond / 1000);
}


u8&
Ne2000::_Memory(u32 address)
{
	static u8 sNowhere;
	address &= 0xffff;
	if (address < sizeof(fProm))
		return fProm[address];
	if (address >= kRamStart && address < kRamEnd)
		return fRam[address - kRamStart];
	sNowhere = 0xff;
	return sNowhere;
}


void
Ne2000::_UpdateIrq()
{
	bool level = (fIsr & fImr & 0x7f) != 0;
	if (level != fIrqLevel) {
		fIrqLevel = level;
		if (fIrq)
			fIrq(level);
	}
}


// #pragma mark - registers


u32
Ne2000::IoRead(u32 port, int size)
{
	int offset = port & 0x1f;
	if (offset >= 0x18) {
		// reading the reset port resets the card
		Reset();
		return 0;
	}
	if (offset >= 0x10)
		return _DataRead(size);
	u32 value = 0;
	for (int i = 0; i < size; i++)
		value |= (u32)_ReadRegister((offset + i) & 0x0f) << (8 * i);
	return value;
}


void
Ne2000::IoWrite(u32 port, u32 value, int size)
{
	int offset = port & 0x1f;
	if (offset >= 0x18)
		return;
	if (offset >= 0x10) {
		_DataWrite((u16)value, size == 1 ? 1 : 2);
		return;
	}
	for (int i = 0; i < size; i++)
		_WriteRegister((offset + i) & 0x0f, (u8)(value >> (8 * i)));
}


u8
Ne2000::_ReadRegister(int offset)
{
	if (offset == 0)
		return fCommand;
	switch (fCommand >> 6) {
		case 0:
			switch (offset) {
				case 0x03: return fBoundary;
				case 0x04: return fTsr;
				case 0x07: return fIsr;
				case 0x08: return (u8)fRemoteAddress;
				case 0x09: return (u8)(fRemoteAddress >> 8);
				case 0x0a: return 0x50;		// RTL8019 ID ("P")
				case 0x0b: return 0x70;
				case 0x0c: return fRsr;
				case 0x0d: case 0x0e: case 0x0f:
				{
					u8 counter = fCounters[offset - 0x0d];
					fCounters[offset - 0x0d] = 0;
					return counter;
				}
			}
			return 0;
		case 1:
			if (offset <= 6)
				return fPar[offset - 1];
			if (offset == 7)
				return fCurrent;
			return fMar[offset - 8];
		case 2:
			switch (offset) {
				case 0x01: return fPageStart;
				case 0x02: return fPageStop;
				case 0x04: return fTransmitPage;
				case 0x0c: return fRcr;
				case 0x0d: return fTcr;
				case 0x0e: return fDcr;
				case 0x0f: return fImr;
			}
			return 0;
	}
	return 0;
}


void
Ne2000::_WriteRegister(int offset, u8 value)
{
	if (offset == 0) {
		_Command(value);
		return;
	}
	switch (fCommand >> 6) {
		case 0:
			switch (offset) {
				case 0x01: fPageStart = value; break;
				case 0x02: fPageStop = value; break;
				case 0x03: fBoundary = value; break;
				case 0x04: fTransmitPage = value; break;
				case 0x05:
					fTransmitCount = (fTransmitCount & 0xff00) | value;
					break;
				case 0x06:
					fTransmitCount = (fTransmitCount & 0x00ff) | (value << 8);
					break;
				case 0x07:
					fIsr &= ~value;
					_UpdateIrq();
					break;
				case 0x08:
					fRemoteAddress = (fRemoteAddress & 0xff00) | value;
					break;
				case 0x09:
					fRemoteAddress = (fRemoteAddress & 0x00ff) | (value << 8);
					break;
				case 0x0a:
					fRemoteCount = (fRemoteCount & 0xff00) | value;
					break;
				case 0x0b:
					fRemoteCount = (fRemoteCount & 0x00ff) | (value << 8);
					break;
				case 0x0c: fRcr = value; break;
				case 0x0d: fTcr = value; break;
				case 0x0e: fDcr = value; break;
				case 0x0f:
					fImr = value;
					_UpdateIrq();
					break;
			}
			break;
		case 1:
			if (offset <= 6)
				fPar[offset - 1] = value;
			else if (offset == 7)
				fCurrent = value;
			else
				fMar[offset - 8] = value;
			break;
	}
}


void
Ne2000::_Command(u8 value)
{
	fCommand = value & ~kCommandTransmit;
	if ((value & kCommandStop) != 0) {
		fCommand &= ~kCommandStart;
		fIsr |= kIsrReset;
	} else if ((value & kCommandStart) != 0)
		fIsr &= ~kIsrReset;
	int dma = (value >> 3) & 7;
	if ((dma == 1 || dma == 2) && fRemoteCount == 0)
		fIsr |= kIsrRemoteDmaDone;
	else if (dma == 3) {
		// "send packet": the host reads the frame at the boundary
		u8 header[4];
		for (int i = 0; i < 4; i++)
			header[i] = _Memory((fBoundary << 8) + i);
		fRemoteAddress = (u16)(fBoundary << 8);
		fRemoteCount = (u16)(header[2] | (header[3] << 8));
	}
	if ((value & kCommandTransmit) != 0 && (value & kCommandStop) == 0)
		_Transmit();
	_UpdateIrq();
}


void
Ne2000::_Transmit()
{
	u32 length = std::min<u32>(fTransmitCount, 1536);
	u8 frame[1536];
	for (u32 i = 0; i < length; i++)
		frame[i] = _Memory((fTransmitPage << 8) + i);
	if ((fTcr & kTcrLoopback) != 0)
		Receive(frame, length);
	else if (fBackend != nullptr)
		fBackend->Send(frame, length);
	BB_LOG(kLogIsa, kLogDebug, "ne2000: sent %u bytes", length);
	fTsr = 0x01;
	fIsr |= kIsrTransmitted;
	_UpdateIrq();
}


// #pragma mark - remote DMA


void
Ne2000::_AdvanceDma(int count)
{
	fRemoteAddress = (u16)(fRemoteAddress + count);
	// reads of the receive ring wrap from PSTOP to PSTART
	if (fPageStop > fPageStart && (fRemoteAddress >> 8) == fPageStop)
		fRemoteAddress = (u16)((fPageStart << 8) | (fRemoteAddress & 0xff));
	fRemoteCount = count >= fRemoteCount ? 0 : (u16)(fRemoteCount - count);
	if (fRemoteCount == 0) {
		fIsr |= kIsrRemoteDmaDone;
		_UpdateIrq();
	}
}


u16
Ne2000::_DataRead(int size)
{
	if (size == 1) {
		u8 value = _Memory(fRemoteAddress);
		_AdvanceDma(1);
		return value;
	}
	// 16-bit: the byte at the even address in bits 7-0
	u16 value = (u16)(_Memory(fRemoteAddress)
		| (_Memory(fRemoteAddress + 1) << 8));
	_AdvanceDma(2);
	return value;
}


void
Ne2000::_DataWrite(u16 value, int size)
{
	u32 address = fRemoteAddress;
	if (address >= kRamStart && address < kRamEnd)
		fRam[address - kRamStart] = (u8)value;
	if (size == 2 && address + 1 >= kRamStart && address + 1 < kRamEnd)
		fRam[address + 1 - kRamStart] = (u8)(value >> 8);
	_AdvanceDma(size);
}


// #pragma mark - receiving


bool
Ne2000::_Accepts(const u8* frame) const
{
	if ((fRcr & kRcrPromiscuous) != 0)
		return true;
	static const u8 kBroadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	if (memcmp(frame, kBroadcast, 6) == 0)
		return (fRcr & kRcrBroadcast) != 0;
	if ((frame[0] & 1) != 0)
		return (fRcr & kRcrMulticast) != 0;
	return memcmp(frame, fPar, 6) == 0;
}


// Pages free between CURR and BNRY (the host's read pointer).
int
Ne2000::_FreePages() const
{
	int ring = fPageStop - fPageStart;
	if (ring <= 0)
		return 0;
	int free = fBoundary - fCurrent;
	if (free <= 0)
		free += ring;
	return free;
}


bool
Ne2000::Receive(const u8* frame, size_t length)
{
	if ((fCommand & kCommandStop) != 0 || length < 14)
		return true;		// card stopped: the frame is lost
	if (!_Accepts(frame))
		return true;
	size_t size = std::max<size_t>(length, kMinFrame);
	size_t total = size + 4;
	int pages = (int)((total + 255) / 256);
	// keep one page free: CURR must not catch up with BNRY
	if (pages >= _FreePages())
		return false;

	int ring = fPageStop - fPageStart;
	int next = fCurrent + pages;
	if (next >= fPageStop)
		next -= ring;
	u8 header[4];
	header[0] = (u8)(0x01 | ((frame[0] & 1) != 0 ? 0x20 : 0));	// PRX, PHY
	header[1] = (u8)next;
	header[2] = (u8)total;
	header[3] = (u8)(total >> 8);
	u32 start = fCurrent << 8;
	u32 ringStart = fPageStart << 8;
	u32 ringEnd = fPageStop << 8;
	for (size_t i = 0; i < total; i++) {
		u8 byte = i < 4 ? header[i] : i - 4 < length ? frame[i - 4] : 0;
		u32 address = start + (u32)i;
		if (address >= ringEnd)
			address -= ringEnd - ringStart;
		_Memory(address) = byte;
	}
	fCurrent = (u8)next;
	fRsr = header[0];
	fIsr |= kIsrReceived;
	_UpdateIrq();
	return true;
}

}	// namespace bebox
