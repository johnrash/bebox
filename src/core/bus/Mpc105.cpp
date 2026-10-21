/*
 * BeBox emulator - Motorola MPC105 "Eagle" PCI bridge / memory controller.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Mpc105.h"

#include "../Log.h"

#include <algorithm>

namespace bebox {

// ISA/PCI I/O: CPU address - 0x80000000 = I/O address. The bridge works in
// big endian mode and keeps byte addresses, so multi-byte values are lane
// swapped for the processor.
class Mpc105::IoWindow : public MmioDevice {
public:
	IoWindow(IoSpace& io, PhysBus& bus) : fIo(io), fBus(bus) {}

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u32 value = fIo.Read(addr - 0x80000000, size, initiator,
			fBus.CurrentPc(initiator));
		return SwapSized(value, size);
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		fIo.Write(addr - 0x80000000, SwapSized(value, size), size, initiator,
			fBus.CurrentPc(initiator));
	}

	virtual const char* MmioName() const { return "isa-io"; }

private:
	IoSpace& fIo;
	PhysBus& fBus;
};


// Direct configuration window: 0x80800000 | IDSEL bit (11-31 -> device
// number) | function << 8 | register.
class Mpc105::ConfigWindow : public MmioDevice {
public:
	ConfigWindow(PciBus& pci) : fPci(pci) {}

	static int Device(u32 addr)
	{
		u32 idsel = addr & 0x007ff800;
		if (idsel == 0)
			return -1;
		return __builtin_ctz(idsel);
	}

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		int device = Device(addr);
		BB_LOG(kLogPci, kLogTrace, "direct config read %08x size %d "
			"(device %d)", addr, size, device);
		if (device < 0)
			return 0xffffffff;
		u32 value = fPci.ConfigRead(device, (addr >> 8) & 7, addr & 0xff, size,
			initiator);
		return SwapSized(value, size);
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		int device = Device(addr);
		if (device < 0)
			return;
		fPci.ConfigWrite(device, (addr >> 8) & 7, addr & 0xff,
			SwapSized(value, size), size);
	}

	virtual const char* MmioName() const { return "pci-config"; }

private:
	PciBus& fPci;
};


class Mpc105::IntAckWindow : public MmioDevice {
public:
	IntAckWindow(Mpc105& bridge) : fBridge(bridge) {}

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		if (addr < 0xbffffff0)
			return 0xffffffff;
		u8 vector = fBridge.fIntAck ? fBridge.fIntAck() : 0xff;
		// The vector is in the least significant byte lane of the PCI
		// interrupt acknowledge cycle, i.e. the first byte here.
		switch (size) {
			case 1: return vector;
			case 2: return (u32)vector << 8;
			default: return (u32)vector << 24;
		}
	}

	virtual void MmioWrite(u32, u32, int, int) {}
	virtual const char* MmioName() const { return "int-ack"; }

private:
	Mpc105& fBridge;
};


// A bank without SIMMs: the data bus floats.
class Mpc105::EmptyBank : public MmioDevice {
public:
	virtual u32 MmioRead(u32, int size, int)
	{
		return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	virtual void MmioWrite(u32, u32, int, int) {}
	virtual const char* MmioName() const { return "empty-bank"; }
};


Mpc105::Mpc105(PhysBus& bus, IoSpace& io, PciBus& pci)
	:
	PciDevice(0x1057, 0x0001, 0x060000, 0x11),
	fBus(bus),
	fIo(io),
	fPci(pci),
	fConfigAddress(0)
{
	fIoWindow = new IoWindow(io, bus);
	fConfigWindow = new ConfigWindow(pci);
	fIntAckWindow = new IntAckWindow(*this);
	fEmptyBank = new EmptyBank;

	fBus.MapDevice(0x80000000, 0x00800000, fIoWindow);
	fBus.MapDevice(0x80800000, 0x00800000, fConfigWindow);
	fBus.MapDevice(0x81000000, 0x3e800000, fIoWindow);
	fBus.MapDevice(0xbffff000, 0x1000, fIntAckWindow);
	fIo.Map(0xcf8, 8, this);
	Reset();
}


Mpc105::~Mpc105()
{
	delete fIoWindow;
	delete fConfigWindow;
	delete fIntAckWindow;
	delete fEmptyBank;
}


void
Mpc105::SetBank(int index, u8* host, u32 size)
{
	// 2 MB: 9 row and 9 column bits, 4 MB: 10 x 9, 8 MB: 10 x 10, 16 MB:
	// 11 x 10, ... 128 MB: 12 x 12 (manual, table 6-2), 8 bytes a column
	Bank& bank = fBanks[index];
	bank.host = host;
	bank.size = size;
	int addressBits = size != 0 ? __builtin_ctz(size) - 3 : 0;
	bank.rowBits = (addressBits + 1) / 2;
	bank.columnBits = addressBits - bank.rowBits;
}


void
Mpc105::Reset()
{
	memset(fConfig + 0x40, 0, sizeof(fConfig) - 0x40);
	fConfig[4] = 0x06;		// memory and bus master enabled
	fConfig[6] = 0x80;		// fast back-to-back capable
	// PICR1: CF_CBA_MASK all ones, RCS0 (ROM on the 60x bus), XATS (map
	// A), CF_LOOP_SNOOP.
	StoreLE32(&fConfig[0xa8], 0xff110010);
	StoreLE32(&fConfig[0xac], 0x000c060c);	// PICR2
	StoreLE32(&fConfig[0xf0], 0xff000000);	// MCCR1
	fConfigAddress = 0;
	memset(fMappedConfig, 0, sizeof(fMappedConfig));
	_UpdateMemoryMap();
}


u32
Mpc105::ConfigRead(u32 reg, int size, int initiator)
{
	u32 value = PciDevice::ConfigRead(reg, size, initiator);
	// PICR1[CF_MP_ID] tells the processors apart.
	if (initiator == kInitiatorCpu1 && reg <= 0xa9 && reg + size > 0xa9)
		value |= 0x80u << (8 * (0xa9 - reg));
	return value;
}


void
Mpc105::ConfigRegisterWritten(u32 reg)
{
	// Read only bits of PICR1: RCS0, XATS, MP_ID.
	u32 picr1 = LoadLE32(&fConfig[0xa8]);
	picr1 = (picr1 & ~0x00118000u) | 0x00110000u;
	StoreLE32(&fConfig[0xa8], picr1);

	if (memcmp(fMappedConfig, &fConfig[0x80], sizeof(fMappedConfig)) != 0
		|| memcmp(fMappedRowBits, &fConfig[0xf0], sizeof(fMappedRowBits))
			!= 0)
		_UpdateMemoryMap();
}


u32
Mpc105::_BankStart(int bank) const
{
	u32 low = fConfig[0x80 + bank];
	u32 high = fConfig[0x88 + bank] & 3;
	return (high << 28) | (low << 20);
}


u32
Mpc105::_BankEnd(int bank) const
{
	u32 low = fConfig[0x90 + bank];
	u32 high = fConfig[0x98 + bank] & 3;
	return (high << 28) | (low << 20) | 0xfffff;
}


void
Mpc105::_UpdateMemoryMap()
{
	memcpy(fMappedConfig, &fConfig[0x80], sizeof(fMappedConfig));
	memcpy(fMappedRowBits, &fConfig[0xf0], sizeof(fMappedRowBits));
	fBus.Unmap(0, 0x40000000);
	u8 enabled = fConfig[0xa0];
	u32 rowConfig = LoadLE16(&fConfig[0xf0]);
	for (int bank = 0; bank < 8; bank++) {
		if ((enabled & (1 << bank)) == 0)
			continue;
		u32 start = _BankStart(bank);
		u32 end = _BankEnd(bank);
		if (end < start)
			continue;
		u32 length = end - start + 1;
		const Bank& memory = fBanks[bank];
		if (memory.host == nullptr || memory.size == 0) {
			fBus.MapDevice(start, length, fEmptyBank);
			continue;
		}
		// The address goes to the DRAMs multiplexed (manual, figure 6-6,
		// 64-bit mode): bits 3-10 are column bits 0-7, the 12 bits above
		// them the row, and the 4 bits above the bank's row bit count in
		// MCCR1 column bits 8-11. The DRAMs decode only the row and column
		// bits they have, so the other address bits alias: the nub sizes
		// each bank with that, first its row bit count, then the columns.
		// A run of 2^11 times the rows decoded is linear.
		int rows = 9 + ((rowConfig >> (2 * bank)) & 3);
		u32 run = 1u << (11 + std::min(rows, memory.rowBits));
		for (u32 address = start; address <= end; ) {
			u32 row = (address >> 11) & ((1u << memory.rowBits) - 1);
			u32 column = (address >> (11 + rows))
				& ((1u << (memory.columnBits - 8)) - 1);
			u32 offset = (address & 0x7ff) | row << 11
				| column << (11 + memory.rowBits);
			u32 chunk = std::min(run - (address & (run - 1)),
				end - address + 1);
			fBus.MapMemory(address, chunk, memory.host + offset, true);
			address += chunk;
		}
		BB_LOG(kLogBus, kLogDebug, "bank %d: %08x-%08x (%u MB, %d x %d, "
			"MCCR1 %d row bits)", bank, start, end, memory.size >> 20,
			memory.rowBits, memory.columnBits, rows);
	}
}


u32
Mpc105::IoRead(u32 port, int size)
{
	if (port >= 0xcf8 && port < 0xcfc) {
		// CONFIG_ADDRESS only answers 32-bit accesses; smaller ones are
		// ordinary I/O cycles (configuration mechanism detection relies
		// on it)
		if (port != 0xcf8 || size != 4)
			return size == 4 ? 0xffffffff : (1u << (8 * size)) - 1;
		return fConfigAddress;
	}
	if ((fConfigAddress & 0x80000000) == 0)
		return 0xffffffff;
	int bus = (fConfigAddress >> 16) & 0xff;
	int device = (fConfigAddress >> 11) & 0x1f;
	int function = (fConfigAddress >> 8) & 7;
	u32 reg = (fConfigAddress & 0xfc) + (port - 0xcfc);
	if (bus != 0)
		return 0xffffffff;
	// CF_MP_ID depends on the processor doing the data cycle.
	return fPci.ConfigRead(device, function, reg, size,
		fIo.CurrentInitiator());
}


void
Mpc105::IoWrite(u32 port, u32 value, int size)
{
	if (port >= 0xcf8 && port < 0xcfc) {
		if (port != 0xcf8 || size != 4)
			return;
		fConfigAddress = value;
		BB_LOG(kLogPci, kLogTrace, "config address %08x", fConfigAddress);
		return;
	}
	if ((fConfigAddress & 0x80000000) == 0)
		return;
	int bus = (fConfigAddress >> 16) & 0xff;
	int device = (fConfigAddress >> 11) & 0x1f;
	int function = (fConfigAddress >> 8) & 7;
	u32 reg = (fConfigAddress & 0xfc) + (port - 0xcfc);
	if (bus != 0)
		return;
	fPci.ConfigWrite(device, function, reg, value, size);
}


void
Mpc105::MasterRead(u32 address, void* buffer, u32 length)
{
	if (address >= kPciMasterMemoryBase && address < kPciMemoryBase) {
		fBus.DmaRead(address - kPciMasterMemoryBase, buffer, length);
		return;
	}
	// peer to peer: PCI memory as the processor sees it
	fBus.DmaRead(address + kPciMemoryBase, buffer, length);
}


void
Mpc105::MasterWrite(u32 address, const void* buffer, u32 length)
{
	if (address >= kPciMasterMemoryBase && address < kPciMemoryBase) {
		fBus.DmaWrite(address - kPciMasterMemoryBase, buffer, length);
		return;
	}
	fBus.DmaWrite(address + kPciMemoryBase, buffer, length);
}

}	// namespace bebox
