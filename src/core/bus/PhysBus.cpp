/*
 * BeBox emulator - physical address space of the processor bus.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "PhysBus.h"

#include "../Log.h"

#include <algorithm>

namespace bebox {

PhysBus::PhysBus()
	:
	fActiveReservations(0),
	fThreaded(false),
	fMapSequence(0),
	fUnclaimedCount(0)
{
	memset(&fEmpty, 0, sizeof(fEmpty));
	for (int i = 0; i < 4096; i++)
		fL1[i] = &fEmpty;
	for (int i = 0; i < kMaxCpus; i++) {
		fReservation[i] = 0;
		fReservationValid[i] = false;
		fReservedValue[i] = 0;
		fCurrentPc[i] = 0;
	}
}


PhysBus::~PhysBus()
{
	for (int i = 0; i < 4096; i++) {
		if (fL1[i] != &fEmpty)
			delete fL1[i];
	}
}


PhysBus::L2Table*
PhysBus::_WritableL2(u32 address)
{
	L2Table*& table = fL1[address >> 20];
	if (table == &fEmpty) {
		table = new L2Table;
		memset(table, 0, sizeof(L2Table));
	}
	return table;
}


void
PhysBus::_SetPages(u32 base, u32 size, const BusPage& page, u8* hostBase)
{
	BusLock lock(*this);
	struct Sequence {
		Sequence(std::atomic<u32>& count) : fCount(count)
			{ fCount.fetch_add(1); }
		~Sequence() { fCount.fetch_add(1); }
		std::atomic<u32>& fCount;
	} sequence(fMapSequence);
	u64 end = (u64)base + size;
	for (u64 address = base & ~kPageMask; address < end;
			address += kPageSize) {
		BusPage& target = _WritableL2((u32)address)
			->pages[(address >> kPageShift) & 0xff];
		target = page;
		if (hostBase != nullptr) {
			u8* host = hostBase + (address - base);
			target.read = host;
			target.write = page.write != nullptr ? host : nullptr;
		}
	}
	_Notify();
}


void
PhysBus::MapMemory(u32 base, u32 size, u8* host, bool writable,
	MmioDevice* writeDevice)
{
	BusPage page;
	page.read = host;
	page.write = writable ? host : nullptr;
	page.device = writeDevice;
	_SetPages(base, size, page, host);
}


void
PhysBus::MapDevice(u32 base, u32 size, MmioDevice* device)
{
	BusPage page = { nullptr, nullptr, device };
	_SetPages(base, size, page, nullptr);
}


void
PhysBus::Unmap(u32 base, u32 size)
{
	BusPage page = { nullptr, nullptr, nullptr };
	_SetPages(base, size, page, nullptr);
}


void
PhysBus::AddListener(BusMapListener* listener)
{
	fListeners.push_back(listener);
}


void
PhysBus::RemoveListener(BusMapListener* listener)
{
	fListeners.erase(std::remove(fListeners.begin(), fListeners.end(),
		listener), fListeners.end());
}


void
PhysBus::_Notify()
{
	for (BusMapListener* listener : fListeners)
		listener->BusMapChanged();
}


void
PhysBus::_Unclaimed(const char* what, u32 address, int size, u32 value,
	int initiator)
{
	fUnclaimedCount++;
	// A guest polling a missing device would flood the log: report the
	// first ones, then only every power of two.
	if (fUnclaimedCount > 100
		&& (fUnclaimedCount & (fUnclaimedCount - 1)) != 0)
		return;
	if (initiator < kMaxCpus) {
		BB_LOG(kLogBus, kLogWarning,
			"unclaimed %s%d at %08x value %08x (cpu%d pc %08x)", what,
			size * 8, address, value, initiator, fCurrentPc[initiator]);
	} else {
		BB_LOG(kLogBus, kLogWarning,
			"unclaimed %s%d at %08x value %08x (dma)", what, size * 8,
			address, value);
	}
}


u32
PhysBus::_DeviceRead(const BusPage& page, u32 address, int size,
	int initiator)
{
	if (page.device == nullptr) {
		u32 value = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
		_Unclaimed("read", address, size, value, initiator);
		return value;
	}
	return page.device->MmioRead(address, size, initiator);
}


void
PhysBus::_DeviceWrite(const BusPage& page, u32 address, u32 value, int size,
	int initiator)
{
	if (page.device == nullptr) {
		_Unclaimed("write", address, size, value, initiator);
		return;
	}
	page.device->MmioWrite(address, value, size, initiator);
}


// A sequence lock: the entry is good if no change began or ended while
// it was read.
BusPage
PhysBus::PageSnapshot(u32 address)
{
	if (!fThreaded)
		return Page(address);
	while (true) {
		u32 before = fMapSequence.load(std::memory_order_acquire);
		if ((before & 1) != 0) {
			CpuRelax();
			continue;
		}
		BusPage page = Page(address);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (fMapSequence.load(std::memory_order_relaxed) == before)
			return page;
	}
}


u8
PhysBus::Read8(u32 address, int initiator)
{
	BusLock lock(*this);
	const BusPage& page = Page(address);
	if (page.read != nullptr)
		return page.read[address & kPageMask];
	return (u8)_DeviceRead(page, address, 1, initiator);
}


u16
PhysBus::Read16(u32 address, int initiator)
{
	if ((address & kPageMask) > kPageSize - 2)
		return (u16)(Read8(address, initiator) << 8)
			| Read8(address + 1, initiator);
	BusLock lock(*this);
	const BusPage& page = Page(address);
	if (page.read != nullptr)
		return LoadBE16(page.read + (address & kPageMask));
	return (u16)_DeviceRead(page, address, 2, initiator);
}


u32
PhysBus::Read32(u32 address, int initiator)
{
	if ((address & kPageMask) > kPageSize - 4) {
		u32 value = 0;
		for (int i = 0; i < 4; i++)
			value = (value << 8) | Read8(address + i, initiator);
		return value;
	}
	BusLock lock(*this);
	const BusPage& page = Page(address);
	if (page.read != nullptr)
		return LoadBE32(page.read + (address & kPageMask));
	return _DeviceRead(page, address, 4, initiator);
}


u64
PhysBus::Read64(u32 address, int initiator)
{
	return ((u64)Read32(address, initiator) << 32)
		| Read32(address + 4, initiator);
}


void
PhysBus::Write8(u32 address, u8 value, int initiator)
{
	SnoopStore(address, 1, initiator);
	BusLock lock(*this);
	const BusPage& page = Page(address);
	if (page.write != nullptr)
		page.write[address & kPageMask] = value;
	else
		_DeviceWrite(page, address, value, 1, initiator);
}


void
PhysBus::Write16(u32 address, u16 value, int initiator)
{
	if ((address & kPageMask) > kPageSize - 2) {
		Write8(address, value >> 8, initiator);
		Write8(address + 1, (u8)value, initiator);
		return;
	}
	SnoopStore(address, 2, initiator);
	BusLock lock(*this);
	const BusPage& page = Page(address);
	if (page.write != nullptr)
		StoreBE16(page.write + (address & kPageMask), value);
	else
		_DeviceWrite(page, address, value, 2, initiator);
}


void
PhysBus::Write32(u32 address, u32 value, int initiator)
{
	if ((address & kPageMask) > kPageSize - 4) {
		for (int i = 0; i < 4; i++)
			Write8(address + i, (u8)(value >> (24 - 8 * i)), initiator);
		return;
	}
	SnoopStore(address, 4, initiator);
	BusLock lock(*this);
	const BusPage& page = Page(address);
	if (page.write != nullptr)
		StoreBE32(page.write + (address & kPageMask), value);
	else
		_DeviceWrite(page, address, value, 4, initiator);
}


void
PhysBus::Write64(u32 address, u64 value, int initiator)
{
	Write32(address, (u32)(value >> 32), initiator);
	Write32(address + 4, (u32)value, initiator);
}


void
PhysBus::DmaRead(u32 address, void* buffer, u32 length)
{
	BusLock lock(*this);
	u8* out = (u8*)buffer;
	while (length > 0) {
		u32 offset = address & kPageMask;
		u32 chunk = std::min(length, kPageSize - offset);
		const BusPage& page = Page(address);
		if (page.read != nullptr) {
			memcpy(out, page.read + offset, chunk);
		} else {
			for (u32 i = 0; i < chunk; i++)
				out[i] = (u8)_DeviceRead(page, address + i, 1, kInitiatorDma);
		}
		out += chunk;
		address += chunk;
		length -= chunk;
	}
}


void
PhysBus::DmaWrite(u32 address, const void* buffer, u32 length)
{
	BusLock lock(*this);
	const u8* in = (const u8*)buffer;
	while (length > 0) {
		u32 offset = address & kPageMask;
		u32 chunk = std::min(length, kPageSize - offset);
		SnoopStore(address, chunk, kInitiatorDma);
		const BusPage& page = Page(address);
		if (page.write != nullptr) {
			memcpy(page.write + offset, in, chunk);
		} else {
			for (u32 i = 0; i < chunk; i++)
				_DeviceWrite(page, address + i, in[i], 1, kInitiatorDma);
		}
		in += chunk;
		address += chunk;
		length -= chunk;
	}
}


// The owner sets and checks its reservation; any processor (or DMA) may
// clear it by storing to the granule.
void
PhysBus::SetReservation(int cpu, u32 address, u32 value)
{
	fReservation[cpu].store(address & ~31u, std::memory_order_relaxed);
	fReservedValue[cpu] = value;
	if (!fReservationValid[cpu].exchange(true))
		fActiveReservations.fetch_add(1);
}


void
PhysBus::ClearReservation(int cpu)
{
	if (fReservationValid[cpu].exchange(false))
		fActiveReservations.fetch_sub(1);
}


bool
PhysBus::CheckReservation(int cpu, u32 address) const
{
	return fReservationValid[cpu].load()
		&& fReservation[cpu].load(std::memory_order_relaxed)
			== (address & ~31u);
}


void
PhysBus::_SnoopStore(u32 address, u32 size, int initiator)
{
	u32 first = address & ~31u;
	u32 last = (address + size - 1) & ~31u;
	for (int cpu = 0; cpu < kMaxCpus; cpu++) {
		// A processor's own stores do not clear its reservation.
		if (cpu == initiator || !fReservationValid[cpu].load())
			continue;
		u32 granule = fReservation[cpu].load(std::memory_order_relaxed);
		if (granule >= first && granule <= last)
			ClearReservation(cpu);
	}
}

}	// namespace bebox
