/*
 * BeBox emulator - PCI bus 0 and configuration space.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "PciBus.h"

#include "../Log.h"

namespace bebox {

PciDevice::PciDevice(u16 vendor, u16 device, u32 classCode, u8 revision)
	:
	fDeviceNumber(-1)
{
	memset(fConfig, 0, sizeof(fConfig));
	StoreLE16(&fConfig[0], vendor);
	StoreLE16(&fConfig[2], device);
	fConfig[8] = revision;
	fConfig[9] = (u8)classCode;
	fConfig[10] = (u8)(classCode >> 8);
	fConfig[11] = (u8)(classCode >> 16);
	for (int i = 0; i < 6; i++) {
		fBarType[i] = kBarNone;
		fBarSize[i] = 0;
	}
}


void
PciDevice::DeclareBar(int index, BarType type, u32 size, bool prefetchable)
{
	fBarType[index] = type;
	fBarSize[index] = size;
	u32 flags = type == kBarIo ? 1 : (prefetchable ? 8 : 0);
	StoreLE32(&fConfig[0x10 + 4 * index], flags);
}


u32
PciDevice::BarAddress(int index) const
{
	u32 value = LoadLE32(&fConfig[0x10 + 4 * index]);
	return fBarType[index] == kBarIo ? value & ~3u : value & ~15u;
}


u32
PciDevice::ConfigRead(u32 reg, int size, int initiator)
{
	u32 value = 0;
	for (int i = 0; i < size; i++)
		value |= (u32)fConfig[(reg + i) & 0xff] << (8 * i);
	return value;
}


void
PciDevice::ConfigWrite(u32 reg, u32 value, int size)
{
	bool barsChanged = false;
	for (int i = 0; i < size; i++) {
		u32 r = (reg + i) & 0xff;
		u8 byte = (u8)(value >> (8 * i));
		if (r < 0x04 || (r >= 0x08 && r < 0x0c) || r == 0x0e || r == 0x3d
			|| (r >= 0x2c && r < 0x30)) {
			continue;	// read only: IDs, class, header type, pin
		}
		if (r == 0x06 || r == 0x07) {
			fConfig[r] &= ~byte;	// status: write one to clear
			continue;
		}
		if (r >= 0x10 && r < 0x28) {
			int bar = (r - 0x10) / 4;
			int offset = (r - 0x10) % 4;
			if (fBarType[bar] == kBarNone)
				continue;
			u32 old = LoadLE32(&fConfig[0x10 + 4 * bar]);
			u32 address = old;
			address &= ~(0xffu << (8 * offset));
			address |= (u32)byte << (8 * offset);
			u32 mask = ~(fBarSize[bar] - 1);
			u32 flags = old & (fBarType[bar] == kBarIo ? 3u : 15u);
			address = (address & mask) | flags;
			StoreLE32(&fConfig[0x10 + 4 * bar], address);
			barsChanged = true;
			continue;
		}
		if (r == 0x04 && ((fConfig[r] ^ byte) & 7) != 0)
			barsChanged = true;
		fConfig[r] = byte;
	}
	if (barsChanged)
		BarsChanged();
	ConfigRegisterWritten(reg);
}


PciBus::PciBus()
{
	for (int i = 0; i < 32; i++)
		fDevices[i] = nullptr;
}


void
PciBus::Attach(int deviceNumber, PciDevice* device)
{
	fDevices[deviceNumber] = device;
	device->SetDeviceNumber(deviceNumber);
}


u32
PciBus::ConfigRead(int device, int function, u32 reg, int size,
	int initiator)
{
	PciDevice* target = Device(device);
	if (target == nullptr || function != 0) {
		BB_LOG(kLogPci, kLogDebug, "config read of empty slot %d.%d reg %02x",
			device, function, reg);
		return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	u32 value = target->ConfigRead(reg, size, initiator);
	BB_LOG(kLogPci, kLogTrace, "config read %s reg %02x size %d = %x",
		target->PciName(), reg, size, value);
	return value;
}


void
PciBus::ConfigWrite(int device, int function, u32 reg, u32 value, int size)
{
	PciDevice* target = Device(device);
	if (target == nullptr || function != 0) {
		BB_LOG(kLogPci, kLogDebug,
			"config write to empty slot %d.%d reg %02x value %x", device,
			function, reg, value);
		return;
	}
	BB_LOG(kLogPci, kLogDebug, "config write %s reg %02x size %d = %x",
		target->PciName(), reg, size, value);
	target->ConfigWrite(reg, value, size);
}

}	// namespace bebox
