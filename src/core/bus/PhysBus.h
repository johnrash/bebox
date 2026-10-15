/*
 * BeBox emulator - physical address space of the processor bus.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The bus is a page granular (4 KB) dispatch table. A page is either plain
 * memory (RAM, ROM, video memory) that CPUs may access through a host
 * pointer, or belongs to an MmioDevice. Read-only memory pages (ROM, flash
 * in read array mode) have a read pointer and send writes to their device.
 *
 * Values exchanged with MmioDevice are in the processor's (big endian)
 * view: a 32-bit read returns the byte at 'addr' in bits 31..24.
 *
 * In threaded mode (a host thread per processor) one recursive spin lock
 * serializes everything but plain memory accesses: device accesses, DMA,
 * page table changes (BusLock). The CPUs read page entries without it
 * (PageSnapshot: a sequence count tells them to read again).
 */
#ifndef BEBOX_PHYS_BUS_H
#define BEBOX_PHYS_BUS_H

#include "../Spin.h"
#include "../Types.h"

#include <atomic>
#include <vector>

namespace bebox {

class MmioDevice {
public:
	virtual ~MmioDevice() {}
	// size is 1, 2 or 4; the address is not necessarily aligned.
	virtual u32 MmioRead(u32 addr, int size, int initiator) = 0;
	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator) = 0;
	virtual const char* MmioName() const = 0;
};

struct BusPage {
	u8* read;				// host pointer to the page, or null
	u8* write;				// host pointer for direct writes, or null
	MmioDevice* device;		// non-direct accesses; null means unmapped
};

class BusMapListener {
public:
	virtual ~BusMapListener() {}
	virtual void BusMapChanged() = 0;
};

class PhysBus {
public:
	static const u32 kPageShift = 12;
	static const u32 kPageSize = 1 << kPageShift;
	static const u32 kPageMask = kPageSize - 1;
	static const int kMaxCpus = 2;

								PhysBus();
								~PhysBus();

			// Maps host memory. 'host' must stay valid while mapped. A null
			// writeDevice with writable == false drops writes (logged).
			void				MapMemory(u32 base, u32 size, u8* host,
									bool writable,
									MmioDevice* writeDevice = nullptr);
			void				MapDevice(u32 base, u32 size,
									MmioDevice* device);
			void				Unmap(u32 base, u32 size);

			const BusPage&		Page(u32 address) const
									{ return fL1[address >> 20]
										->pages[(address >> kPageShift)
											& 0xff]; }

			// Slow path accesses (any page type). 8 byte accesses to
			// devices are split into two 4 byte accesses.
			u8					Read8(u32 address, int initiator);
			u16					Read16(u32 address, int initiator);
			u32					Read32(u32 address, int initiator);
			u64					Read64(u32 address, int initiator);
			void				Write8(u32 address, u8 value, int initiator);
			void				Write16(u32 address, u16 value,
									int initiator);
			void				Write32(u32 address, u32 value,
									int initiator);
			void				Write64(u32 address, u64 value,
									int initiator);

			// Block transfers for bus masters (DMA). They snoop the
			// processors' reservations. Device pages are accessed bytewise.
			void				DmaRead(u32 address, void* buffer,
									u32 length);
			void				DmaWrite(u32 address, const void* buffer,
									u32 length);

			// lwarx/stwcx. reservations (32 byte granules) and the value
			// lwarx loaded (threaded stwcx. compares and swaps it).
			void				SetReservation(int cpu, u32 address,
									u32 value = 0);
			void				ClearReservation(int cpu);
			bool				CheckReservation(int cpu, u32 address) const;
			u32					ReservedValue(int cpu) const
									{ return fReservedValue[cpu]; }
			// Called for every store; cheap when no reservation is held.
			void				SnoopStore(u32 address, u32 size,
									int initiator)
									{
										if (fActiveReservations.load(
												std::memory_order_relaxed)
												!= 0)
											_SnoopStore(address, size,
												initiator);
									}

			// Threaded mode: see above.
			void				SetThreaded(bool threaded)
									{ fThreaded = threaded; }
			bool				IsThreaded() const { return fThreaded; }
			RecursiveSpinLock&	DeviceLock() { return fLock; }
			// The page's entry, read consistently in threaded mode
			// (devices may remap pages meanwhile).
			BusPage				PageSnapshot(u32 address);

			void				AddListener(BusMapListener* listener);
			void				RemoveListener(BusMapListener* listener);

			// For log messages about unclaimed accesses.
			void				SetCurrentPc(int initiator, u32 pc)
									{ if (initiator < kMaxCpus)
										fCurrentPc[initiator] = pc; }
			u32					CurrentPc(int initiator) const
									{ return initiator < kMaxCpus
										? fCurrentPc[initiator] : 0; }

			u64					UnclaimedAccessCount() const
									{ return fUnclaimedCount; }

private:
	struct L2Table {
		BusPage pages[256];
	};

			L2Table*			_WritableL2(u32 address);
			void				_SetPages(u32 base, u32 size,
									const BusPage& page, u8* hostBase);
			void				_Notify();
			void				_SnoopStore(u32 address, u32 size,
									int initiator);
			u32					_DeviceRead(const BusPage& page,
									u32 address, int size, int initiator);
			void				_DeviceWrite(const BusPage& page,
									u32 address, u32 value, int size,
									int initiator);
			void				_Unclaimed(const char* what, u32 address,
									int size, u32 value, int initiator);

			L2Table*			fL1[4096];
			L2Table				fEmpty;
			std::vector<BusMapListener*> fListeners;

			std::atomic<u32>	fReservation[kMaxCpus];
			std::atomic<bool>	fReservationValid[kMaxCpus];
			u32					fReservedValue[kMaxCpus];
			std::atomic<int>	fActiveReservations;
			bool				fThreaded;
			RecursiveSpinLock	fLock;
			// odd while the page tables change (with fLock held)
			std::atomic<u32>	fMapSequence;
			u32					fCurrentPc[kMaxCpus];
			u64					fUnclaimedCount;
};

// Holds the bus lock in threaded mode.
class BusLock {
public:
	explicit BusLock(PhysBus& bus)
		:
		fBus(bus),
		fLocked(bus.IsThreaded())
	{
		if (fLocked)
			fBus.DeviceLock().lock();
	}

	~BusLock()
	{
		if (fLocked)
			fBus.DeviceLock().unlock();
	}

private:
	PhysBus&	fBus;
	bool		fLocked;
};

}	// namespace bebox

#endif	// BEBOX_PHYS_BUS_H
