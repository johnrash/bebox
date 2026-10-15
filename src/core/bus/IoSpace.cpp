/*
 * BeBox emulator - ISA/PCI I/O port space.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "IoSpace.h"

#include "../Log.h"

#include <algorithm>

namespace bebox {

IoSpace::IoSpace()
	:
	fUnclaimed(0),
	fInitiator(0)
{
	for (u32 i = 0; i < 0x10000; i++)
		fLow[i] = nullptr;
}


void
IoSpace::Map(u32 first, u32 count, IoDevice* device)
{
	if (first < 0x10000 && first + count <= 0x10000) {
		for (u32 i = 0; i < count; i++)
			fLow[first + i] = device;
		return;
	}
	Unmap(first, count);
	fHigh.push_back(Range{ first, count, device });
}


void
IoSpace::Unmap(u32 first, u32 count)
{
	if (first < 0x10000 && first + count <= 0x10000) {
		for (u32 i = 0; i < count; i++)
			fLow[first + i] = nullptr;
		return;
	}
	fHigh.erase(std::remove_if(fHigh.begin(), fHigh.end(),
		[first, count](const Range& range) {
			return range.first < first + count
				&& first < range.first + range.count;
		}), fHigh.end());
}


IoDevice*
IoSpace::DeviceAt(u32 port) const
{
	if (port < 0x10000)
		return fLow[port];
	for (const Range& range : fHigh) {
		if (port - range.first < range.count)
			return range.device;
	}
	return nullptr;
}


u32
IoSpace::Read(u32 port, int size, int initiator, u32 pc)
{
	fInitiator = initiator;
	IoDevice* device = DeviceAt(port);
	if (device != nullptr && (size == 1 || DeviceAt(port + size - 1) == device))
		return device->IoRead(port, size);
	if (device == nullptr && (size == 1 || DeviceAt(port + size - 1) == nullptr)) {
		fUnclaimed++;
		BB_LOG(kLogIsa, kLogInfo, "unclaimed I/O read%d %04x (cpu%d pc %08x)",
			size * 8, port, initiator, pc);
		return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	// spans devices: byte by byte
	u32 value = 0;
	for (int i = 0; i < size; i++)
		value |= Read(port + i, 1, initiator, pc) << (8 * i);
	return value;
}


void
IoSpace::Write(u32 port, u32 value, int size, int initiator, u32 pc)
{
	fInitiator = initiator;
	IoDevice* device = DeviceAt(port);
	if (device != nullptr && (size == 1 || DeviceAt(port + size - 1) == device)) {
		device->IoWrite(port, value, size);
		return;
	}
	if (device == nullptr && (size == 1 || DeviceAt(port + size - 1) == nullptr)) {
		fUnclaimed++;
		BB_LOG(kLogIsa, kLogInfo,
			"unclaimed I/O write%d %04x value %x (cpu%d pc %08x)", size * 8,
			port, value, initiator, pc);
		return;
	}
	for (int i = 0; i < size; i++)
		Write(port + i, (value >> (8 * i)) & 0xff, 1, initiator, pc);
}

}	// namespace bebox
